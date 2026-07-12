// macos_support.hpp
// Pure decisions used by the macOS platform layer. These helpers intentionally
// contain no Apple framework types so they can be covered by hardware-free CI.
#pragma once

#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/hid_descriptor.hpp"
#include "sony_head_tracker/hid_usages.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace sony {

struct ReportIntervalChoice {
    std::int64_t raw{};
    double seconds{};
};

inline std::optional<ReportIntervalChoice> chooseReportInterval(
    const DescriptorField& field) {
    const auto scale = std::pow(10.0, static_cast<double>(field.unitExponent));
    const auto lowSeconds = static_cast<double>(field.physicalMin) * scale;
    const auto highSeconds = static_cast<double>(field.physicalMax) * scale;
    if (highSeconds <= lowSeconds || field.logicalMax <= field.logicalMin) {
        return std::nullopt;
    }
    auto targetSeconds = std::max(0.010, lowSeconds);
    if (targetSeconds > 0.020 || highSeconds < 0.010) targetSeconds = lowSeconds;
    targetSeconds = std::clamp(targetSeconds, lowSeconds, highSeconds);
    const auto physicalTarget = targetSeconds / scale;
    const auto fraction = (physicalTarget - field.physicalMin) /
                          static_cast<double>(field.physicalMax - field.physicalMin);
    auto raw = std::clamp(
        std::llround(field.logicalMin + fraction * (field.logicalMax - field.logicalMin)),
        static_cast<long long>(field.logicalMin),
        static_cast<long long>(field.logicalMax));
    if (raw == 0 && field.logicalMax >= 1) {
        const auto candidateSeconds = descriptorScale(
            1, field.logicalMin, field.logicalMax,
            field.physicalMin, field.physicalMax, field.unitExponent);
        if (candidateSeconds >= 0.010 && candidateSeconds <= 0.020) {
            raw = 1;
            targetSeconds = candidateSeconds;
        }
    }
    return ReportIntervalChoice{raw, targetSeconds};
}

inline bool isVerifiedAndroidTracker(const DeviceInfo& device) {
    return device.usagePage == 0x20 && device.usage == 0xE1 &&
           device.androidHeadTracker;
}

inline unsigned reconnectBackoffSeconds(std::size_t attempt) {
    constexpr std::array<unsigned, 5> delays{1, 2, 5, 10, 30};
    return delays[std::min(attempt, delays.size() - 1)];
}

enum class StreamRecoveryAction {
    refreshServices,
    reopenHid,
};

// A stalled configured stream may need one SDP refresh, but it must not drop
// the headset's Bluetooth baseband connection. Continued stalls recycle only
// the IOHID and silent-audio sessions.
inline StreamRecoveryAction streamRecoveryAction(std::size_t consecutiveTimeouts) {
    if (consecutiveTimeouts <= 1) return StreamRecoveryAction::refreshServices;
    return StreamRecoveryAction::reopenHid;
}

inline unsigned streamReconnectBackoffSeconds(std::size_t attempt) {
    constexpr std::array<unsigned, 2> delays{1, 2};
    return delays[std::min(attempt, delays.size() - 1)];
}

inline bool trackerAvailabilityBecameReady(
    bool previousBluetoothConnected,
    bool previousHidVisible,
    bool bluetoothConnected,
    bool hidVisible) {
    return (!previousBluetoothConnected && bluetoothConnected) ||
           (!previousHidVisible && hidVisible);
}

// The raw feature-report fallback operates on reports returned by an untrusted
// HID device.  Keep its byte layout and every bit-range check in this pure
// helper so the platform code cannot infer structure from report contents.
struct FeatureReportLayout {
    std::uint8_t reportId{};
    bool hasReportIdPrefix{};
};

inline std::optional<FeatureReportLayout> featureReportLayoutFor(
    std::span<const DescriptorField> fields, std::uint8_t reportId) {
    bool hasTarget{};
    bool hasZeroId{};
    bool hasNonzeroId{};
    for (const auto& field : fields) {
        if (!field.feature) continue;
        hasTarget = hasTarget || field.reportId == reportId;
        hasZeroId = hasZeroId || field.reportId == 0;
        hasNonzeroId = hasNonzeroId || field.reportId != 0;
    }
    // A HID descriptor either uses report IDs for every report or for none.
    // A mixed zero/nonzero layout cannot be represented safely by this writer.
    if (!hasTarget || (hasZeroId && hasNonzeroId)) return std::nullopt;
    return FeatureReportLayout{reportId, reportId != 0};
}

inline bool canConfigureFeatureReports(const DeviceInfo& device) {
    if (!isVerifiedAndroidTracker(device)) return false;
    constexpr std::array requiredUsages{
        kPowerFull, static_cast<std::uint16_t>(0x0855),
        kReportingAllEvents, static_cast<std::uint16_t>(0x0840),
        kReportInterval,
    };
    for (const auto usage : requiredUsages) {
        const auto field = std::find_if(device.fields.begin(), device.fields.end(),
                                        [&](const auto& candidate) {
            return candidate.feature && candidate.usagePage == kSensorPage &&
                   candidate.usage == usage;
        });
        if (field == device.fields.end() || field->bitSize == 0 ||
            field->bitSize > 64 || field->reportCount == 0) {
            return false;
        }
    }
    for (const auto& field : device.fields) {
        if (field.feature && !featureReportLayoutFor(device.fields, field.reportId)) return false;
    }
    return true;
}

inline bool featureBitRangeFits(std::size_t reportBytes,
                                const FeatureReportLayout& layout,
                                std::size_t bitOffset,
                                std::size_t bitWidth) {
    if (bitWidth == 0 || bitWidth > 64) return false;
    constexpr auto bitsPerByte = std::size_t{8};
    if (reportBytes > std::numeric_limits<std::size_t>::max() / bitsPerByte) return false;
    const auto prefixBits = layout.hasReportIdPrefix ? bitsPerByte : std::size_t{};
    const auto reportBits = reportBytes * bitsPerByte;
    if (prefixBits > reportBits || bitOffset > std::numeric_limits<std::size_t>::max() - prefixBits) return false;
    const auto firstBit = prefixBits + bitOffset;
    return firstBit <= reportBits && bitWidth <= reportBits - firstBit;
}

inline std::optional<std::uint64_t> readFeatureBits(
    std::span<const std::uint8_t> report, const FeatureReportLayout& layout,
    std::size_t bitOffset, std::size_t bitWidth) {
    if (!featureBitRangeFits(report.size(), layout, bitOffset, bitWidth)) return std::nullopt;
    const auto firstBit = (layout.hasReportIdPrefix ? std::size_t{8} : std::size_t{}) + bitOffset;
    std::uint64_t value{};
    for (std::size_t bit = 0; bit < bitWidth; ++bit) {
        const auto absolute = firstBit + bit;
        if ((report[absolute / 8] >> (absolute % 8)) & 1u) value |= std::uint64_t{1} << bit;
    }
    return value;
}

inline bool writeFeatureBits(std::span<std::uint8_t> report,
                             const FeatureReportLayout& layout,
                             std::size_t bitOffset, std::size_t bitWidth,
                             std::uint64_t value) {
    if (!featureBitRangeFits(report.size(), layout, bitOffset, bitWidth)) return false;
    const auto firstBit = (layout.hasReportIdPrefix ? std::size_t{8} : std::size_t{}) + bitOffset;
    for (std::size_t bit = 0; bit < bitWidth; ++bit) {
        const auto absolute = firstBit + bit;
        const auto mask = static_cast<std::uint8_t>(1u << (absolute % 8));
        if ((value >> bit) & 1u) report[absolute / 8] |= mask;
        else report[absolute / 8] &= static_cast<std::uint8_t>(~mask);
    }
    return true;
}

enum class BluetoothConnectionWaitResult { connected, openFailed, timedOut, cancelled };

inline bool bluetoothConnectionConfirmed(BluetoothConnectionWaitResult result,
                                         bool currentlyConnected) {
    return result == BluetoothConnectionWaitResult::connected && currentlyConnected;
}

template <typename IsConnected, typename Pump, typename IsCancelled, typename Now>
BluetoothConnectionWaitResult waitForBluetoothConnection(
    bool openSucceeded, IsConnected&& isConnected, Pump&& pump,
    IsCancelled&& isCancelled, Now&& now,
    std::chrono::milliseconds timeout = std::chrono::seconds(2),
    std::chrono::milliseconds slice = std::chrono::milliseconds(50)) {
    if (!openSucceeded) return BluetoothConnectionWaitResult::openFailed;
    if (isConnected()) return BluetoothConnectionWaitResult::connected;
    const auto deadline = now() + timeout;
    while (true) {
        if (isCancelled()) return BluetoothConnectionWaitResult::cancelled;
        const auto current = now();
        if (current >= deadline) return BluetoothConnectionWaitResult::timedOut;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - current);
        pump(remaining.count() > 0 ? std::min(slice, remaining) : std::chrono::milliseconds(1));
        if (isConnected()) return BluetoothConnectionWaitResult::connected;
    }
}

} // namespace sony

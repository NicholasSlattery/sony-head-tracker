// macos_support.hpp
// Pure decisions used by the macOS platform layer. These helpers intentionally
// contain no Apple framework types so they can be covered by hardware-free CI.
#pragma once

#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/hid_descriptor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

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
    forceBasebandReconnect,
    reopenHid,
};

// Escalate a stalled, configured stream only once per step. Continued stalls
// recycle IOHID with bounded backoff instead of repeatedly dropping Bluetooth.
inline StreamRecoveryAction streamRecoveryAction(std::size_t consecutiveTimeouts) {
    if (consecutiveTimeouts <= 1) return StreamRecoveryAction::refreshServices;
    if (consecutiveTimeouts == 2) return StreamRecoveryAction::forceBasebandReconnect;
    return StreamRecoveryAction::reopenHid;
}

} // namespace sony

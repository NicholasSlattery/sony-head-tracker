#include "sony_head_tracker_c.h"

#include "sony_head_tracker/audio_wake.hpp"
#include "sony_head_tracker/app_config.hpp"
#include "sony_head_tracker/bluetooth_recovery.hpp"
#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/hid_backend.hpp"
#include "sony_head_tracker/logger.hpp"
#include "sony_head_tracker/macos_support.hpp"
#include "sony_head_tracker/orientation.hpp"
#include "sony_head_tracker/output_udp.hpp"
#include "sony_head_tracker/version.hpp"

#import <Foundation/Foundation.h>
#include <IOKit/hidsystem/IOHIDLib.h>

#include <sys/utsname.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

std::string utf8(std::wstring_view input) {
    std::string output;
    for (const auto character : input) {
        const auto codePoint = static_cast<std::uint32_t>(character);
        if (codePoint <= 0x7F) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint <= 0x10FFFF) {
            output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }
    return output;
}

SHTSample makeCsample(const sony::MotionSample& sample) {
    SHTSample result{};
    result.rotation_vector[0] = sample.rotationVector.x;
    result.rotation_vector[1] = sample.rotationVector.y;
    result.rotation_vector[2] = sample.rotationVector.z;
    result.quaternion[0] = sample.orientation.w;
    result.quaternion[1] = sample.orientation.x;
    result.quaternion[2] = sample.orientation.y;
    result.quaternion[3] = sample.orientation.z;
    result.ypr_degrees[0] = sample.euler.yaw;
    result.ypr_degrees[1] = sample.euler.pitch;
    result.ypr_degrees[2] = sample.euler.roll;
    if (sample.angularVelocity) {
        result.has_gyroscope = true;
        result.gyroscope[0] = sample.angularVelocity->x;
        result.gyroscope[1] = sample.angularVelocity->y;
        result.gyroscope[2] = sample.angularVelocity->z;
    }
    if (sample.acceleration) {
        result.has_accelerometer = true;
        result.accelerometer[0] = sample.acceleration->x;
        result.accelerometer[1] = sample.acceleration->y;
        result.accelerometer[2] = sample.acceleration->z;
    }
    result.reset_counter = sample.resetCounter;
    result.packets_per_second = sample.packetsPerSecond;
    result.receive_latency_ms = sample.receiveLatencyMs;
    return result;
}

bool validFilter(double smoothing,
                 const unsigned source[3],
                 const double sign[3]) {
    if (!source || !sign || !std::isfinite(smoothing)) return false;
    std::array<bool, 3> used{};
    for (std::size_t index = 0; index < 3; ++index) {
        if (source[index] > 2 || used[source[index]] || !std::isfinite(sign[index]) ||
            (sign[index] != -1.0 && sign[index] != 1.0)) {
            return false;
        }
        used[source[index]] = true;
    }
    return true;
}

} // namespace

struct SHTHandle {
    std::mutex lifecycleMutex;
    std::mutex filterMutex;
    std::mutex diagnosticsMutex;
    sony::OrientationFilter filter;
    sony::HidBackend hid;
    sony::UdpOutput udp;
    sony::SilentAudioWake audioWake;
    std::jthread worker;
    std::atomic_bool running{};
    std::atomic_bool recenterRequested{};
    SHTSampleCallback sampleCallback{};
    SHTStatusCallback statusCallback{};
    void* callbackContext{};
    std::chrono::steady_clock::time_point lastUiCallback{};
    std::chrono::steady_clock::time_point lastSampleAt{};
    sony::FilterConfig diagnosticFilter{};
    std::uint16_t diagnosticPort{4242};
    std::size_t candidateCount{};
    std::size_t inputElementCount{};
    std::size_t featureElementCount{};
    std::uint16_t inputReportBytes{};
    std::uint16_t featureReportBytes{};
    std::string descriptorMarker{"(none)"};
    std::string ioHidAccess{"not checked"};
    std::string lastErrorCategory{"none"};
    std::string lastError{"(none)"};
    std::uint64_t reconnectAttempts{};
    double lastPacketRate{-1.0};
};

namespace {

void notifyStatus(SHTHandle& handle, SHTStatus status, std::string message) noexcept {
    if (status == SHT_STATUS_CONNECTED) {
        std::lock_guard lock(handle.diagnosticsMutex);
        handle.lastErrorCategory = "none";
        handle.lastError = "(none)";
    } else if (status == SHT_STATUS_PERMISSION_DENIED ||
               status >= SHT_STATUS_NOT_VISIBLE) {
        std::lock_guard lock(handle.diagnosticsMutex);
        if (status == SHT_STATUS_PERMISSION_DENIED) handle.lastErrorCategory = "permission";
        else if (status == SHT_STATUS_NOT_VISIBLE) handle.lastErrorCategory = "notVisible";
        else if (status == SHT_STATUS_NOT_VERIFIED) handle.lastErrorCategory = "notVerified";
        else if (status == SHT_STATUS_FEATURE_WRITE_FAILED) handle.lastErrorCategory = "featureWrite";
        else if (status == SHT_STATUS_STREAM_TIMEOUT) handle.lastErrorCategory = "streamTimeout";
        else if (status == SHT_STATUS_UDP_ERROR) handle.lastErrorCategory = "udp";
        else handle.lastErrorCategory = "internal";
        handle.lastError = message;
    }
    if (!handle.statusCallback) return;
    try {
        handle.statusCallback(status, message.c_str(), handle.callbackContext);
    } catch (...) {
        // A foreign callback must never unwind through the C ABI or engine thread.
    }
}

void deliverSample(SHTHandle& handle, sony::MotionSample sample,
                   std::atomic_bool& firstSampleReported,
                   std::wstring_view productName) {
    sony::MotionSample filtered;
    {
        std::lock_guard lock(handle.filterMutex);
        if (handle.recenterRequested.exchange(false)) handle.filter.recenter();
        filtered = handle.filter.process(std::move(sample));
    }
    handle.udp.send(filtered);
    {
        std::lock_guard lock(handle.diagnosticsMutex);
        handle.lastSampleAt = std::chrono::steady_clock::now();
        handle.lastPacketRate = filtered.packetsPerSecond;
    }
    if (!firstSampleReported.exchange(true)) {
        notifyStatus(handle, SHT_STATUS_CONNECTED,
                     std::string("Connected: ") + utf8(productName));
    }
    if (!handle.sampleCallback) return;
    const auto now = std::chrono::steady_clock::now();
    if (handle.lastUiCallback.time_since_epoch().count() != 0 &&
        now - handle.lastUiCallback < 33ms) {
        return;
    }
    handle.lastUiCallback = now;
    const auto output = makeCsample(filtered);
    try {
        handle.sampleCallback(&output, handle.callbackContext);
    } catch (...) {
        // A foreign callback must never unwind through the engine thread.
    }
}

enum class ReconnectWaitResult {
    elapsed,
    availabilityChanged,
    stopped,
};

ReconnectWaitResult waitForReconnect(
    std::stop_token stop,
    std::chrono::seconds duration,
    std::wstring_view trackedAddress,
    std::wstring_view trackedProduct,
    bool wakeOnCurrentlyVisibleHid) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    auto previous = sony::queryPairedTrackerAvailability(
        trackedAddress, trackedProduct);
    if (wakeOnCurrentlyVisibleHid && previous.hidCollectionVisible) {
        return ReconnectWaitResult::availabilityChanged;
    }
    auto nextAvailabilityCheck = std::chrono::steady_clock::now() + 250ms;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextAvailabilityCheck) {
            const auto current = sony::queryPairedTrackerAvailability(
                trackedAddress, trackedProduct);
            if (sony::trackerAvailabilityBecameReady(
                    previous.bluetoothConnected,
                    previous.hidCollectionVisible,
                    current.bluetoothConnected,
                    current.hidCollectionVisible)) {
                return ReconnectWaitResult::availabilityChanged;
            }
            previous = current;
            nextAvailabilityCheck = now + 250ms;
        }
        std::this_thread::sleep_for(50ms);
    }
    return stop.stop_requested() ? ReconnectWaitResult::stopped
                                 : ReconnectWaitResult::elapsed;
}

void runEngine(SHTHandle& handle, std::stop_token stop,
               std::uint16_t basePort) noexcept {
    try {
        const auto hidAccess = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
        const bool hidAccessGranted = hidAccess == kIOHIDAccessTypeGranted ||
            (hidAccess != kIOHIDAccessTypeDenied &&
             IOHIDRequestAccess(kIOHIDRequestTypeListenEvent));
        {
            std::lock_guard lock(handle.diagnosticsMutex);
            handle.ioHidAccess = hidAccessGranted ? "granted" : "denied";
        }
        if (!hidAccessGranted) {
            notifyStatus(
                handle,
                SHT_STATUS_PERMISSION_DENIED,
                "Input Monitoring is required. Open System Settings → Privacy & Security → Input Monitoring, enable Sony Head Tracker, then restart the app."
            );
            handle.running = false;
            return;
        }
        if (!handle.udp.open("127.0.0.1", basePort)) {
            notifyStatus(handle, SHT_STATUS_UDP_ERROR,
                         "Could not open loopback UDP output; choose another base port");
            handle.running = false;
            return;
        }
        std::wstring trackedAddress = sony::loadLastVerifiedBluetoothAddress();
        std::wstring trackedProduct;
        std::size_t availabilityBackoffIndex{};
        std::size_t streamBackoffIndex{};
        unsigned bluetoothRecoveryStage{};
        std::size_t consecutiveStreamTimeouts{};
        bool streamRecoveryPending{};
        notifyStatus(handle, SHT_STATUS_SCANNING, "Scanning for Android Head Tracker");

        while (!stop.stop_requested()) {
            auto devices = handle.hid.enumerate();
            {
                std::lock_guard lock(handle.diagnosticsMutex);
                handle.candidateCount = devices.size();
            }
            if (streamRecoveryPending) {
                const auto action = sony::streamRecoveryAction(consecutiveStreamTimeouts);
                if (action != sony::StreamRecoveryAction::reopenHid &&
                    (!trackedAddress.empty() || !trackedProduct.empty())) {
                    notifyStatus(
                        handle,
                        SHT_STATUS_RECONNECTING,
                        "Recycling IOHID and refreshing the paired headset HID services");
                    sony::recoverPairedBluetoothHid(
                        trackedAddress, trackedProduct, false, stop);
                    devices = handle.hid.enumerate();
                    {
                        std::lock_guard lock(handle.diagnosticsMutex);
                        handle.candidateCount = devices.size();
                    }
                } else {
                    notifyStatus(handle, SHT_STATUS_RECONNECTING,
                                 "Reopening the IOHID backend after a stalled stream");
                }
                streamRecoveryPending = false;
            }
            auto selected = std::find_if(devices.begin(), devices.end(),
                                         [](const auto& device) {
                                             return sony::isVerifiedAndroidTracker(device);
                                         });
            if (selected == devices.end() && bluetoothRecoveryStage < 2 &&
                (!trackedAddress.empty() || !trackedProduct.empty())) {
                notifyStatus(handle, SHT_STATUS_RECONNECTING,
                             bluetoothRecoveryStage == 0
                                 ? "Refreshing the paired headset HID services"
                                 : "Reconnecting the paired headset and refreshing HID services");
                const auto recovery = sony::recoverPairedBluetoothHid(
                    trackedAddress, trackedProduct, bluetoothRecoveryStage == 1, stop);
                if (recovery.pairedDeviceFound && recovery.connected) {
                    ++bluetoothRecoveryStage;
                    devices = handle.hid.enumerate();
                    {
                        std::lock_guard lock(handle.diagnosticsMutex);
                        handle.candidateCount = devices.size();
                    }
                    selected = std::find_if(devices.begin(), devices.end(),
                                            [](const auto& device) {
                                                return sony::isVerifiedAndroidTracker(device);
                                            });
                }
            }

            if (selected == devices.end()) {
                const auto unverified = std::any_of(
                    devices.begin(), devices.end(), [](const auto& device) {
                        return device.usagePage == 0x20 && device.usage == 0xE1;
                    });
                notifyStatus(handle,
                             unverified ? SHT_STATUS_NOT_VERIFIED : SHT_STATUS_NOT_VISIBLE,
                             unverified
                                 ? "A head-tracker collection is visible but its Android marker was not verified"
                                 : "Android Head Tracker is not currently visible");
                {
                    std::lock_guard lock(handle.diagnosticsMutex);
                    ++handle.reconnectAttempts;
                }
                const auto delay = sony::reconnectBackoffSeconds(
                    availabilityBackoffIndex);
                if (availabilityBackoffIndex < 4) ++availabilityBackoffIndex;
                const auto waitResult = waitForReconnect(
                    stop, std::chrono::seconds(delay), trackedAddress,
                    trackedProduct, !unverified);
                if (waitResult == ReconnectWaitResult::stopped) break;
                if (waitResult == ReconnectWaitResult::availabilityChanged) {
                    notifyStatus(handle, SHT_STATUS_RECONNECTING,
                                 "Paired headset or IOHID collection became available; retrying now");
                }
                continue;
            }

            trackedAddress = selected->bluetoothAddress;
            trackedProduct = selected->product;
            {
                std::lock_guard lock(handle.diagnosticsMutex);
                handle.descriptorMarker = selected->sensorDescription.empty()
                    ? "(none)" : selected->sensorDescription;
                handle.inputElementCount = std::count_if(
                    selected->fields.begin(), selected->fields.end(),
                    [](const auto& field) { return !field.feature; });
                handle.featureElementCount = selected->fields.size() - handle.inputElementCount;
                handle.inputReportBytes = selected->inputReportBytes;
                handle.featureReportBytes = selected->featureReportBytes;
            }
            if (!trackedAddress.empty()) {
                sony::saveLastVerifiedBluetoothAddress(trackedAddress);
            }
            handle.udp.setDeviceLabel(trackedProduct);
            std::atomic_bool firstSampleReported{};
            handle.lastUiCallback = {};
            const auto connected = handle.hid.connect(
                *selected, {},
                [&handle, &firstSampleReported, product = trackedProduct](auto sample) {
                    deliverSample(handle, std::move(sample), firstSampleReported, product);
                });
            if (!connected) {
                notifyStatus(handle, SHT_STATUS_FEATURE_WRITE_FAILED,
                             "The verified tracker rejected feature configuration; include the feature read-back log in a report");
            } else {
                handle.audioWake.start(trackedProduct, trackedAddress);
                const auto configuredAt = std::chrono::steady_clock::now();
                bool healthyStream{};
                bool streamTimedOut{};
                while (!stop.stop_requested() && handle.hid.connected()) {
                    if (firstSampleReported.load() && !healthyStream) {
                        healthyStream = true;
                        consecutiveStreamTimeouts = 0;
                        bluetoothRecoveryStage = 0;
                        availabilityBackoffIndex = 0;
                        streamBackoffIndex = 0;
                    }
                    if (!firstSampleReported.load() &&
                        std::chrono::steady_clock::now() - configuredAt >= 5s) {
                        bool waitingForSample = false;
                        if (!firstSampleReported.compare_exchange_strong(
                                waitingForSample, true)) {
                            continue;
                        }
                        // The timeout now owns this connection's terminal
                        // transition. A callback arriving concurrently cannot
                        // publish Connected immediately before teardown.
                        streamTimedOut = true;
                        ++consecutiveStreamTimeouts;
                        streamRecoveryPending = true;
                        notifyStatus(handle, SHT_STATUS_STREAM_TIMEOUT,
                                     "Tracker configured but no valid sample arrived; recycling the IOHID backend automatically");
                        break;
                    }
                    std::this_thread::sleep_for(100ms);
                }
                if (streamTimedOut) {
                    notifyStatus(handle, SHT_STATUS_RECONNECTING,
                                 "Stream stalled; closing the current IOHID session before retrying");
                }
            }
            handle.audioWake.stop();
            handle.hid.disconnect();
            if (!stop.stop_requested()) {
                {
                    std::lock_guard lock(handle.diagnosticsMutex);
                    ++handle.reconnectAttempts;
                }
                const bool stalledStream = streamRecoveryPending;
                const auto delay = stalledStream
                    ? sony::streamReconnectBackoffSeconds(streamBackoffIndex)
                    : sony::reconnectBackoffSeconds(availabilityBackoffIndex);
                if (stalledStream) {
                    if (streamBackoffIndex < 1) ++streamBackoffIndex;
                } else if (availabilityBackoffIndex < 4) {
                    ++availabilityBackoffIndex;
                }
                if (connected) {
                    notifyStatus(
                        handle,
                        SHT_STATUS_RECONNECTING,
                        stalledStream
                            ? std::format("Stalled stream closed; retrying in {} second(s)", delay)
                            : std::format("Tracker disconnected; retrying in {} second(s)", delay));
                }
                const auto waitResult = waitForReconnect(
                    stop, std::chrono::seconds(delay), trackedAddress,
                    trackedProduct, connected && !stalledStream);
                if (waitResult == ReconnectWaitResult::stopped) break;
                if (waitResult == ReconnectWaitResult::availabilityChanged) {
                    notifyStatus(handle, SHT_STATUS_RECONNECTING,
                                 "Paired headset or IOHID collection became available; retrying now");
                }
            }
        }
        handle.audioWake.stop();
        handle.hid.disconnect();
        handle.udp.close();
        handle.running = false;
        notifyStatus(handle, SHT_STATUS_STOPPED, "Stopped");
    } catch (const std::exception& error) {
        handle.audioWake.stop();
        handle.hid.disconnect();
        handle.udp.close();
        handle.running = false;
        notifyStatus(handle, SHT_STATUS_ERROR, error.what());
    } catch (...) {
        handle.audioWake.stop();
        handle.hid.disconnect();
        handle.udp.close();
        handle.running = false;
        notifyStatus(handle, SHT_STATUS_ERROR, "Unknown engine error");
    }
}

void stopHandle(SHTHandle& handle) noexcept {
    try {
        std::unique_lock lock(handle.lifecycleMutex);
        if (!handle.worker.joinable()) {
            handle.running = false;
            return;
        }
        handle.worker.request_stop();
        lock.unlock();
        handle.worker.join();
        lock.lock();
        handle.running = false;
    } catch (...) {
        handle.running = false;
    }
}

} // namespace

extern "C" SHTHandle *sht_create(void) {
    try {
        return new (std::nothrow) SHTHandle;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void sht_destroy(SHTHandle *handle) {
    if (!handle) return;
    try {
        stopHandle(*handle);
        delete handle;
    } catch (...) {
        // Destruction is best-effort and must never throw across C.
    }
}

extern "C" bool sht_start(SHTHandle *handle,
                           uint16_t base_port,
                           SHTSampleCallback sample_callback,
                           SHTStatusCallback status_callback,
                           void *context) {
    if (!handle || base_port == 0 || base_port == UINT16_MAX) return false;
    try {
        std::unique_lock lock(handle->lifecycleMutex);
        if (handle->worker.joinable()) {
            if (handle->running) return false;
            auto completedWorker = std::move(handle->worker);
            lock.unlock();
            completedWorker.join();
            lock.lock();
        }
        if (handle->running.exchange(true)) return false;
        handle->sampleCallback = sample_callback;
        handle->statusCallback = status_callback;
        handle->callbackContext = context;
        handle->recenterRequested = false;
        {
            std::lock_guard diagnosticsLock(handle->diagnosticsMutex);
            handle->diagnosticPort = base_port;
            handle->lastErrorCategory = "none";
            handle->lastError = "(none)";
            handle->reconnectAttempts = 0;
            handle->lastPacketRate = -1.0;
            handle->lastSampleAt = {};
        }
        handle->worker = std::jthread(
            [handle, base_port](std::stop_token stop) {
                runEngine(*handle, stop, base_port);
            });
        return true;
    } catch (...) {
        handle->running = false;
        return false;
    }
}

extern "C" void sht_stop(SHTHandle *handle) {
    if (!handle) return;
    stopHandle(*handle);
}

extern "C" void sht_recenter(SHTHandle *handle) {
    if (!handle) return;
    try {
        handle->recenterRequested = true;
    } catch (...) {
    }
}

extern "C" void sht_set_filter(SHTHandle *handle,
                                double smoothing,
                                const unsigned axis_source[3],
                                const double axis_sign[3]) {
    if (!handle || !validFilter(smoothing, axis_source, axis_sign)) return;
    try {
        sony::FilterConfig config;
        config.smoothing = std::clamp(smoothing, 0.01, 1.0);
        for (std::size_t index = 0; index < 3; ++index) {
            config.axes.source[index] = axis_source[index];
            config.axes.sign[index] = axis_sign[index];
        }
        std::lock_guard lock(handle->filterMutex);
        handle->filter.setConfig(config);
        {
            std::lock_guard diagnosticsLock(handle->diagnosticsMutex);
            handle->diagnosticFilter = config;
        }
    } catch (...) {
    }
}

extern "C" size_t sht_get_diagnostics(SHTHandle *handle,
                                        char *buffer,
                                        size_t capacity) {
    if (!handle) return 0;
    try {
        std::string access;
        std::string marker;
        std::string category;
        std::string error;
        sony::FilterConfig filter;
        std::size_t candidates{};
        std::size_t inputElements{};
        std::size_t featureElements{};
        std::uint16_t inputBytes{};
        std::uint16_t featureBytes{};
        std::uint16_t port{};
        std::uint64_t reconnects{};
        double packetRate{};
        double sampleAgeMs{-1.0};
        {
            std::lock_guard lock(handle->diagnosticsMutex);
            access = handle->ioHidAccess;
            marker = handle->descriptorMarker;
            category = handle->lastErrorCategory;
            error = handle->lastError;
            filter = handle->diagnosticFilter;
            candidates = handle->candidateCount;
            inputElements = handle->inputElementCount;
            featureElements = handle->featureElementCount;
            inputBytes = handle->inputReportBytes;
            featureBytes = handle->featureReportBytes;
            port = handle->diagnosticPort;
            reconnects = handle->reconnectAttempts;
            packetRate = handle->lastPacketRate;
            if (handle->lastSampleAt.time_since_epoch().count() != 0) {
                sampleAgeMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - handle->lastSampleAt).count();
            }
        }

        std::string osVersion;
        @autoreleasepool {
            const char* version = [[[NSProcessInfo processInfo]
                operatingSystemVersionString] UTF8String];
            if (version) osVersion = version;
        }
        utsname systemInfo{};
        const std::string architecture = uname(&systemInfo) == 0
            ? systemInfo.machine : "unknown";

        std::string featureReadback{"(not available yet)"};
        for (const auto& wideLine : sony::Logger::instance().history()) {
            const auto line = utf8(wideLine);
            if (line.find("readback=") != std::string::npos ||
                line.find("Feature report") != std::string::npos ||
                line.find("configuration accepted") != std::string::npos) {
                featureReadback = line;
            }
        }

        const auto signText = [](double sign) { return sign < 0 ? '-' : '+'; };
        const auto axisText = [](unsigned axis) {
            return axis == 0 ? 'X' : axis == 1 ? 'Y' : 'Z';
        };
        const auto diagnostics = std::format(
            "Sony Head Tracker diagnostics (shareable)\n"
            "App version: {}\nmacOS: {}\nArchitecture: {}\n"
            "IOHID listen access: {}\nCandidate count: {}\n"
            "Product/transport: [verified compatible headset] / macOS IOHID\n"
            "Usage: 0x0020:0x00E1\nDescriptor: {}\n"
            "Elements: input={} feature={} report-bytes={}/{}\n"
            "Feature read-back: {}\nPacket rate: {:.1f} pps\nSample age: {:.1f} ms\n"
            "UDP: 127.0.0.1:{} / {} packets\nReconnect attempts: {}\n"
            "Settings: {}{} {}{} {}{}, smoothing={:.2f}, base-port={}\n"
            "Last error: {} / {}\n",
            utf8(sony::kVersion), osVersion, architecture, access, candidates,
            marker, inputElements, featureElements, inputBytes, featureBytes,
            featureReadback, packetRate, sampleAgeMs, port, handle->udp.packetsSent(),
            reconnects,
            signText(filter.axes.sign[0]), axisText(filter.axes.source[0]),
            signText(filter.axes.sign[1]), axisText(filter.axes.source[1]),
            signText(filter.axes.sign[2]), axisText(filter.axes.source[2]),
            filter.smoothing, port, category, error);
        const auto required = diagnostics.size() + 1;
        if (buffer && capacity != 0) {
            const auto count = std::min(diagnostics.size(), capacity - 1);
            std::memcpy(buffer, diagnostics.data(), count);
            buffer[count] = '\0';
        }
        return required;
    } catch (...) {
        if (buffer && capacity != 0) buffer[0] = '\0';
        return 0;
    }
}

extern "C" bool sht_load_config(SHTConfig *config) {
    if (!config) return false;
    try {
        const auto stored = sony::loadAppConfig();
        config->smoothing = stored.smoothing;
        config->udp_port = stored.udpPort;
        for (std::size_t index = 0; index < 3; ++index) {
            config->axis_source[index] = stored.axes.source[index];
            config->axis_sign[index] = stored.axes.sign[index];
        }
        return true;
    } catch (...) {
        return false;
    }
}

extern "C" bool sht_save_config(const SHTConfig *config) {
    if (!config || config->udp_port == 0 || config->udp_port == UINT16_MAX ||
        !validFilter(config->smoothing, config->axis_source, config->axis_sign)) {
        return false;
    }
    try {
        auto stored = sony::loadAppConfig();
        stored.smoothing = std::clamp(config->smoothing, 0.01, 1.0);
        stored.udpPort = config->udp_port;
        for (std::size_t index = 0; index < 3; ++index) {
            stored.axes.source[index] = config->axis_source[index];
            stored.axes.sign[index] = config->axis_sign[index];
        }
        return sony::saveAppConfig(stored);
    } catch (...) {
        return false;
    }
}

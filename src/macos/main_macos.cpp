// main_macos.cpp
// macOS CLI for probing, dumping, filtering, UDP streaming, recentering, and
// reconnecting the Android Head Tracker IOHID collection.
#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/audio_wake.hpp"
#include "sony_head_tracker/bluetooth_recovery.hpp"
#include "sony_head_tracker/hid_backend.hpp"
#include "sony_head_tracker/logger.hpp"
#include "sony_head_tracker/macos_support.hpp"
#include "sony_head_tracker/orientation.hpp"
#include "sony_head_tracker/output_udp.hpp"
#include "sony_head_tracker/version.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern char** environ;

namespace {

std::atomic_bool stopRequested{};
std::atomic_bool recenterRequested{};
constexpr char kCommandStartEnvironment[] = "SONY_HEAD_TRACKER_COMMAND_START_NS";
constexpr char kProcessRecoveryEnvironment[] = "SONY_HEAD_TRACKER_PROCESS_RECOVERY";

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) stopRequested = true;
    if (signal == SIGUSR1) recenterRequested = true;
}

void installSignals() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGUSR1, signalHandler);
}

void printUsage() {
    std::wcout
        << L"Sony Head Tracker for macOS " << sony::kVersion << L"\n"
        << L"Usage:\n"
        << L"  sony-head-tracker-macos probe\n"
        << L"  sony-head-tracker-macos dump [--seconds N]\n"
        << L"  sony-head-tracker-macos bridge [--port 4242] [--seconds N]\n"
        << L"                                  [--axis-map YXZ] [--invert XZ]\n"
        << L"                                  [--smoothing 0.18]\n"
        << L"  sony-head-tracker-macos help\n"
        << L"  sony-head-tracker-macos version\n";
}

void printDevice(const sony::DeviceInfo& device) {
    std::wcout << std::format(
        L"HID {}\n  manufacturer={} product={} transport=macOS IOHID\n"
        L"  usage=0x{:04X}:0x{:04X}, VID/PID={:04X}:{:04X}, input={} feature={}\n"
        L"  description={}\n  verified Android tracker={}\n",
        device.path, device.manufacturer, device.product,
        device.usagePage, device.usage, device.vendorId, device.productId,
        device.inputReportBytes, device.featureReportBytes,
        std::wstring(device.sensorDescription.begin(), device.sensorDescription.end()),
        device.androidHeadTracker ? L"yes" : L"no");
    for (const auto& field : device.fields) {
        std::wcout << std::format(
            L"    {} report={} usage=0x{:04X}:0x{:04X} count={} bits={} "
            L"logical={}..{} physical={}..{} exponent={}\n",
            field.feature ? L"feature" : L"input", field.reportId,
            field.usagePage, field.usage, field.reportCount, field.bitSize,
            field.logicalMin, field.logicalMax, field.physicalMin,
            field.physicalMax, field.unitExponent);
    }
}

unsigned parseUnsigned(std::string_view text) {
    std::string value(text);
    char* end = nullptr;
    const auto parsed = std::strtoul(value.c_str(), &end, 10);
    return end == value.c_str() || *end != '\0' ? 0 : static_cast<unsigned>(parsed);
}

bool deadlineReached(std::chrono::steady_clock::time_point deadline, unsigned seconds) {
    return seconds != 0 && std::chrono::steady_clock::now() >= deadline;
}

std::chrono::steady_clock::time_point commandStartFromEnvironment() {
    const auto now = std::chrono::steady_clock::now();
    const auto* text = std::getenv(kCommandStartEnvironment);
    if (!text) return now;
    char* end = nullptr;
    const auto count = std::strtoll(text, &end, 10);
    unsetenv(kCommandStartEnvironment);
    if (end == text || *end != '\0' || count <= 0) return now;
    const auto restored = std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(count)));
    return restored <= now ? restored : now;
}

bool waitInterruptibly(std::chrono::seconds duration,
                       std::chrono::steady_clock::time_point deadline,
                       unsigned seconds) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (!stopRequested && !deadlineReached(deadline, seconds) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !stopRequested && !deadlineReached(deadline, seconds);
}

std::vector<sony::DeviceInfo>::const_iterator verifiedDevice(
    const std::vector<sony::DeviceInfo>& devices) {
    return std::find_if(devices.begin(), devices.end(),
                        [](const auto& device) { return sony::isVerifiedAndroidTracker(device); });
}

int runProbe() {
    sony::HidBackend hid;
    const auto devices = hid.enumerate();
    for (const auto& device : devices) printDevice(device);
    const auto selected = verifiedDevice(devices);
    if (selected == devices.end()) {
        std::wcout << L"\nNo verified Android Head Tracker HID sensor was found.\n";
        return 2;
    }
    std::wcout << L"\nVerified Android head tracker found.\n";
    return 0;
}

int runDump(int argc, char** argv) {
    unsigned seconds{};
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--seconds" && index + 1 < argc) {
            seconds = parseUnsigned(argv[++index]);
        } else {
            std::wcerr << L"Invalid dump option. Use 'help'.\n";
            return 1;
        }
    }
    sony::HidBackend hid;
    sony::SilentAudioWake audioWake;
    std::atomic_bool sampleReceived{};
    const auto devices = hid.enumerate();
    const auto selected = verifiedDevice(devices);
    if (selected == devices.end()) {
        std::wcerr << L"No verified Android Head Tracker HID sensor was found.\n";
        return 2;
    }
    for (const auto& device : devices) printDevice(device);
    if (!hid.connect(
            *selected,
            [](const auto& bytes) { std::wcout << sony::hexDump(bytes) << L'\n'; },
            [&](sony::MotionSample sample) {
                sampleReceived = true;
                std::wcout << std::format(
                    L"SAMPLE rotation=({:.6f},{:.6f},{:.6f}) rate={:.1f}pps",
                    sample.rotationVector.x, sample.rotationVector.y,
                    sample.rotationVector.z, sample.packetsPerSecond);
                if (sample.angularVelocity) {
                    std::wcout << std::format(
                        L" gyro=({:.6f},{:.6f},{:.6f})",
                        sample.angularVelocity->x, sample.angularVelocity->y,
                        sample.angularVelocity->z);
                }
                std::wcout << L'\n';
            })) {
        std::wcerr << L"Could not connect/configure the verified Android head tracker.\n";
        return 3;
    }
    if (!sampleReceived) {
        audioWake.start(selected->product, selected->bluetoothAddress);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!stopRequested && !deadlineReached(deadline, seconds) && hid.connected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    audioWake.stop();
    hid.disconnect();
    return 0;
}

int runBridge(int argc, char** argv) {
    std::uint16_t port = 4242;
    unsigned seconds{};
    sony::FilterConfig config;
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--port" && index + 1 < argc) {
            const auto value = parseUnsigned(argv[++index]);
            if (value < 1 || value > 65534) {
                std::wcerr << L"--port must be between 1 and 65534.\n";
                return 1;
            }
            port = static_cast<std::uint16_t>(value);
        } else if (option == "--seconds" && index + 1 < argc) {
            seconds = parseUnsigned(argv[++index]);
        } else if (option == "--smoothing" && index + 1 < argc) {
            std::string value(argv[++index]);
            char* end = nullptr;
            const auto parsed = std::strtod(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0') return 1;
            config.smoothing = std::clamp(parsed, 0.01, 1.0);
        } else if (option == "--invert" && index + 1 < argc) {
            const std::string axes(argv[++index]);
            config.axes.sign = {1.0, 1.0, 1.0};
            for (const auto axis : axes) {
                const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(axis)));
                if (lower == 'x') config.axes.sign[0] = -1.0;
                else if (lower == 'y') config.axes.sign[1] = -1.0;
                else if (lower == 'z') config.axes.sign[2] = -1.0;
                else if (axes != "none") return 1;
            }
        } else if (option == "--axis-map" && index + 1 < argc) {
            const std::string map(argv[++index]);
            if (map.size() != 3) return 1;
            std::array<bool, 3> used{};
            for (unsigned output = 0; output < 3; ++output) {
                const auto axis = static_cast<char>(std::tolower(static_cast<unsigned char>(map[output])));
                const unsigned source = axis == 'x' ? 0 : axis == 'y' ? 1 : axis == 'z' ? 2 : 3;
                if (source > 2 || used[source]) return 1;
                used[source] = true;
                config.axes.source[output] = source;
            }
        } else {
            std::wcerr << L"Invalid bridge option. Use 'help'.\n";
            return 1;
        }
    }

    sony::UdpOutput udp;
    if (!udp.open("127.0.0.1", port)) {
        std::wcerr << L"Could not open UDP output.\n";
        return 4;
    }
    sony::OrientationFilter filter(config);
    std::mutex filterMutex;
    sony::HidBackend hid;
    sony::SilentAudioWake audioWake;
    std::atomic_bool sampleReceived{};
    std::wstring trackedBluetoothAddress = sony::loadLastVerifiedBluetoothAddress();
    std::wstring trackedProductName;
    unsigned bluetoothRecoveryStage{};
    const bool startedByProcessRecovery =
        std::getenv(kProcessRecoveryEnvironment) != nullptr;
    unsetenv(kProcessRecoveryEnvironment);
    const auto commandStart = commandStartFromEnvironment();
    const auto deadline = commandStart + std::chrono::seconds(seconds);

    std::wcout << std::format(
        L"Streaming head tracking:\n  OpenTrack -> UDP 127.0.0.1:{}\n"
        L"  JSON      -> UDP 127.0.0.1:{}\n"
        L"Recenter: kill -USR1 {}\nStop: Ctrl+C\n",
        port, port + 1, static_cast<long>(getpid()));

    const auto output = [&](sony::MotionSample sample) {
        sampleReceived = true;
        std::lock_guard lock(filterMutex);
        auto filtered = filter.process(std::move(sample));
        udp.send(filtered);
        std::wcout << std::format(L"\rYPR {:7.2f} {:7.2f} {:7.2f}  {:5.1f} pps   ",
                                  filtered.euler.yaw, filtered.euler.pitch,
                                  filtered.euler.roll, filtered.packetsPerSecond)
                   << std::flush;
    };

    const auto connect = [&]() {
        auto devices = hid.enumerate();
        auto selected = verifiedDevice(devices);
        if (selected == devices.end() && bluetoothRecoveryStage < 2 &&
            (!trackedBluetoothAddress.empty() || !trackedProductName.empty())) {
            const bool forceBasebandReconnect = bluetoothRecoveryStage == 1;
            std::wcerr << (forceBasebandReconnect
                ? L"Android Head Tracker collection is still absent; forcing one paired-device baseband reconnect and SDP refresh.\n"
                : L"Android Head Tracker collection is absent; requesting paired-device connection and SDP refresh.\n");
            const auto recovery = sony::recoverPairedBluetoothHid(
                trackedBluetoothAddress, trackedProductName, forceBasebandReconnect);
            std::wcerr << std::format(
                L"Bluetooth recovery paired={} wasConnected={} connected={} forced={} "
                L"close=0x{:08X} open=0x{:08X} sdpStart=0x{:08X} sdpCompleted={}\n",
                recovery.pairedDeviceFound, recovery.wasConnected,
                recovery.connected, recovery.forcedBasebandReconnect,
                static_cast<unsigned>(recovery.closeStatus),
                static_cast<unsigned>(recovery.openStatus),
                static_cast<unsigned>(recovery.sdpStartStatus),
                recovery.sdpQueryCompleted);
            if (recovery.pairedDeviceFound && recovery.connected) {
                bluetoothRecoveryStage = forceBasebandReconnect ? 2 : 1;
                devices = hid.enumerate();
                selected = verifiedDevice(devices);
            }
        }
        if (selected == devices.end()) return false;
        trackedBluetoothAddress = selected->bluetoothAddress;
        trackedProductName = selected->product;
        if (!trackedBluetoothAddress.empty() &&
            !sony::saveLastVerifiedBluetoothAddress(trackedBluetoothAddress)) {
            std::wcerr << L"Warning: could not persist the verified tracker identity for future power-cycle recovery.\n";
        }
        bluetoothRecoveryStage = 0;
        const auto& name = selected->product;
        udp.setDeviceLabel(name);
        if (!name.empty()) std::wcout << L"Tracking headset: " << name << L'\n';
        const auto connected = hid.connect(*selected, {}, output);
        if (connected && !sampleReceived) {
            std::wcerr << L"No head-tracker sample yet; starting a silent audio keepalive on the verified headset to activate A2DP.\n";
            if (!audioWake.start(trackedProductName, trackedBluetoothAddress)) {
                std::wcerr << L"Silent A2DP keepalive could not be started; continuing with HID recovery.\n";
            }
        }
        return connected;
    };

    if (deadlineReached(deadline, seconds)) {
        std::wcout << L"Deadline reached during process-level recovery.\n";
        return 0;
    }
    const bool initiallyConnected = connect();
    if (!initiallyConnected && !startedByProcessRecovery &&
        trackedBluetoothAddress.empty()) {
        std::wcerr << L"No verified Android Head Tracker was found.\n";
        return 3;
    }
    if (!initiallyConnected) {
        std::wcerr << (startedByProcessRecovery
            ? L"Process-level recovery could not immediately reacquire the tracker; entering reconnect backoff.\n"
            : L"The previously verified tracker is not yet visible; entering Bluetooth/HID recovery backoff.\n");
    }

    constexpr auto reconnectStreamTimeout = std::chrono::seconds(5);
    std::size_t backoffIndex{};
    bool waitingForReconnectSample{initiallyConnected};
    bool waitingForInitialSample{initiallyConnected && !startedByProcessRecovery};
    bool processRecoveryAttempted{startedByProcessRecovery};
    bool recoveryTimeoutReported{};
    auto reconnectConfiguredAt = std::chrono::steady_clock::now();
    if (startedByProcessRecovery && initiallyConnected) {
        if (sampleReceived) {
            waitingForReconnectSample = false;
            processRecoveryAttempted = false;
            std::wcerr << L"Reconnected; first valid sample received after process recovery.\n";
        } else {
            std::wcerr << L"Process-level IOHID recovery configured; waiting for the first valid sample.\n";
        }
    } else if (initiallyConnected && !sampleReceived) {
        std::wcerr << L"Tracker configured; waiting for the first valid sample with silent A2DP keepalive.\n";
    }
    while (!stopRequested && !deadlineReached(deadline, seconds)) {
        if (recenterRequested.exchange(false)) {
            std::lock_guard lock(filterMutex);
            filter.recenter();
            std::wcerr << L"\nRecenter requested.\n";
        }
        if (waitingForReconnectSample) {
            if (sampleReceived) {
                waitingForReconnectSample = false;
                processRecoveryAttempted = false;
                recoveryTimeoutReported = false;
                backoffIndex = 0;
                if (waitingForInitialSample) {
                    std::wcerr << L"\nFirst valid sample received with silent A2DP keepalive active.\n";
                } else {
                    std::wcerr << L"\nReconnected; first valid sample received.\n";
                }
                waitingForInitialSample = false;
            } else if (std::chrono::steady_clock::now() - reconnectConfiguredAt >=
                           reconnectStreamTimeout && !recoveryTimeoutReported) {
                if (!processRecoveryAttempted) {
                    processRecoveryAttempted = true;
                    std::wcerr << L"No valid sample arrived within 5 seconds after tracker configuration; "
                                  L"starting one clean recovery process.\n";
                    const auto startNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        commandStart.time_since_epoch()).count();
                    const auto startText = std::to_string(startNanoseconds);
                    if (seconds != 0) {
                        setenv(kCommandStartEnvironment, startText.c_str(), 1);
                    }
                    setenv(kProcessRecoveryEnvironment, "1", 1);
                    audioWake.stop();
                    hid.disconnect();
                    udp.close();
                    pid_t recoveryProcess{};
                    const auto spawnError = posix_spawnp(
                        &recoveryProcess, argv[0], nullptr, nullptr, argv, environ);
                    unsetenv(kCommandStartEnvironment);
                    unsetenv(kProcessRecoveryEnvironment);
                    if (spawnError == 0) {
                        int childStatus{};
                        pid_t waited{};
                        do {
                            waited = waitpid(recoveryProcess, &childStatus, 0);
                        } while (waited < 0 && errno == EINTR);
                        if (waited < 0) {
                            const std::string errorText(std::strerror(errno));
                            std::wcerr << L"Could not wait for the recovery process: "
                                       << std::wstring(errorText.begin(), errorText.end()) << L".\n";
                            return 5;
                        }
                        if (WIFEXITED(childStatus)) return WEXITSTATUS(childStatus);
                        if (WIFSIGNALED(childStatus)) return 128 + WTERMSIG(childStatus);
                        return 5;
                    }
                    recoveryTimeoutReported = true;
                    const std::string errorText(std::strerror(spawnError));
                    std::wcerr << L"Automatic recovery process failed to start: "
                               << std::wstring(errorText.begin(), errorText.end())
                               << L". Continuing to wait for input.\n";
                } else {
                    recoveryTimeoutReported = true;
                    std::wcerr << L"Process-level recovery still has no valid sample; "
                                  L"continuing to wait without repeated restarts.\n";
                }
            }
        }
        if (!hid.connected()) {
            recoveryTimeoutReported = false;
            audioWake.stop();
            hid.disconnect();
            const auto delay = sony::reconnectBackoffSeconds(backoffIndex);
            if (backoffIndex < 4) ++backoffIndex;
            std::wcerr << std::format(L"\nDisconnected; reconnecting in {} second(s).\n", delay);
            if (!waitInterruptibly(std::chrono::seconds(delay), deadline, seconds)) break;
            sampleReceived = false;
            if (connect()) {
                backoffIndex = 0;
                waitingForReconnectSample = true;
                waitingForInitialSample = false;
                reconnectConfiguredAt = std::chrono::steady_clock::now();
                if (sampleReceived) {
                    waitingForReconnectSample = false;
                    std::wcerr << L"Reconnected; first valid sample received.\n";
                } else {
                    std::wcerr << L"Tracker configured; waiting for the first valid sample with silent A2DP keepalive.\n";
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    audioWake.stop();
    hid.disconnect();
    std::wcout << std::format(L"\nStopped after {} UDP sample(s).\n", udp.packetsSent());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view command = argc > 1 ? argv[1] : "help";
    if (command == "help" || command == "--help" || command == "-h") {
        printUsage();
        return 0;
    }
    if (command == "version" || command == "--version") {
        std::wcout << L"sony-head-tracker " << sony::kVersion << L"\n";
        return 0;
    }
    installSignals();
    if (command == "probe") return runProbe();
    if (command == "dump") return runDump(argc, argv);
    if (command == "bridge") return runBridge(argc, argv);
    std::wcerr << L"Unknown command. Use 'help'.\n";
    return 1;
}

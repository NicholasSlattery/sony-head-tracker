// bluetooth_recovery.hpp
// Narrow, macOS-only recovery boundary for asking the public IOBluetooth stack
// to reconnect and refresh services for the exact paired headset previously
// identified by IOHID. No Objective-C types cross this interface.
#pragma once

#include <cstdint>
#include <stop_token>
#include <string_view>

namespace sony {

struct BluetoothRecoveryResult {
    bool pairedDeviceFound{};
    bool wasConnected{};
    bool connected{};
    bool forcedBasebandReconnect{};
    bool connectionTimedOut{};
    bool cancelled{};
    bool sdpQueryStarted{};
    bool sdpQueryCompleted{};
    std::int32_t closeStatus{};
    std::int32_t openStatus{};
    std::int32_t sdpStartStatus{};
};

struct TrackerAvailability {
    bool pairedDeviceFound{};
    bool bluetoothConnected{};
    bool hidCollectionVisible{};
};

BluetoothRecoveryResult recoverPairedBluetoothHid(
    std::wstring_view bluetoothAddress,
    std::wstring_view fallbackProductName,
    bool forceBasebandReconnect,
    std::stop_token stop = {});

// Read-only availability check for the exact previously verified paired
// headset. Used to wake reconnect backoff without changing Bluetooth state.
TrackerAvailability queryPairedTrackerAvailability(
    std::wstring_view bluetoothAddress,
    std::wstring_view fallbackProductName);

std::wstring loadLastVerifiedBluetoothAddress();
bool saveLastVerifiedBluetoothAddress(std::wstring_view bluetoothAddress);

} // namespace sony

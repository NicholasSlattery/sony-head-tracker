// bluetooth_recovery.hpp
// Narrow, macOS-only recovery boundary for asking the public IOBluetooth stack
// to reconnect and refresh services for the exact paired headset previously
// identified by IOHID. No Objective-C types cross this interface.
#pragma once

#include <cstdint>
#include <string_view>

namespace sony {

struct BluetoothRecoveryResult {
    bool pairedDeviceFound{};
    bool wasConnected{};
    bool connected{};
    bool forcedBasebandReconnect{};
    bool sdpQueryStarted{};
    bool sdpQueryCompleted{};
    std::int32_t closeStatus{};
    std::int32_t openStatus{};
    std::int32_t sdpStartStatus{};
};

BluetoothRecoveryResult recoverPairedBluetoothHid(
    std::wstring_view bluetoothAddress,
    std::wstring_view fallbackProductName,
    bool forceBasebandReconnect);

std::wstring loadLastVerifiedBluetoothAddress();
bool saveLastVerifiedBluetoothAddress(std::wstring_view bluetoothAddress);

} // namespace sony

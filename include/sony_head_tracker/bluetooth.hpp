// bluetooth.hpp
// Read-only Bluetooth investigation and the driver-rebind recovery ("Repair
// Tracker") entry points, plus the paired-headset name resolution the HID
// backend uses. Signatures expose no Windows types.
#pragma once

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace sony {

struct BluetoothProbeOptions {
    std::wstring_view nameFilter{};   // empty = probe every paired device
    bool probeAllLeDevices{};
};

// Performs read-only Bluetooth Classic SDP and BLE GATT discovery.
// Returns 0 when a matching paired Classic device was queried, 2 when none matched.
int runBluetoothProbe(const BluetoothProbeOptions& options, std::wostream& output);
// An empty nameFilter auto-detects the headset whose SDP record carries the
// Android Head Tracker HID descriptor, so no other paired device is touched.
int rebindBluetoothHid(std::wstring_view nameFilter, std::wostream& output);
int useGenericHidDriver(std::wostream& output);

// Resolves the paired Bluetooth headset name owning a head-tracker HID node.
std::wstring bluetoothNameForHidInstance(std::wstring_view instanceId);
std::vector<std::wstring> pairedBluetoothDeviceNames();

} // namespace sony

// device.hpp
// Plain-data descriptions of a discovered HID collection or Windows Sensor API
// sensor. These are produced by the platform discovery/backends but contain no
// Windows types, so tests and pure code can construct and inspect them freely.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sony {

struct DescriptorField {
    std::uint16_t usagePage{};
    std::uint16_t usage{};
    std::uint8_t reportId{};
    std::uint16_t reportCount{};
    std::uint16_t bitSize{};
    std::int32_t logicalMin{};
    std::int32_t logicalMax{};
    std::int32_t physicalMin{};
    std::int32_t physicalMax{};
    std::int8_t unitExponent{};
    std::uint32_t unit{};
    std::uint16_t dataIndex{};
    bool feature{};
};

struct DeviceInfo {
    std::wstring path;
    std::wstring instanceId;
    std::wstring product;
    std::wstring manufacturer;
    std::wstring bluetoothName;   // paired Bluetooth device name, resolved via the PnP parent chain
    std::uint16_t usagePage{};
    std::uint16_t usage{};
    std::uint16_t vendorId{};
    std::uint16_t productId{};
    std::uint16_t version{};
    std::uint16_t inputReportBytes{};
    std::uint16_t featureReportBytes{};
    std::string sensorDescription;
    std::vector<DescriptorField> fields;
    std::vector<std::string> featureValues;
    bool androidHeadTracker{};
    bool accessDenied{};
};

struct SensorInfo {
    std::wstring friendlyName;
    std::wstring description;
    std::wstring id;
    std::wstring type;
    bool androidHeadTracker{};
};

} // namespace sony

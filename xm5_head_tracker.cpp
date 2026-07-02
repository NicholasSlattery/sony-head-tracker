// xm5_head_tracker.cpp
// =============================================================================
// XM5 Head Tracker Bridge -- single-file edition
//
// A native Windows 11 bridge that turns any headset implementing the Android
// Head Tracker HID protocol (Sony WH-1000XM5 and others) into usable
// head-tracking data. It discovers the headset's HID top-level collection (or
// the Windows Sensor API custom sensor), validates the "#AndroidHeadTracker#"
// marker, resolves which paired Bluetooth headset the sensor belongs to,
// enables reporting, parses orientation, and:
//   * shows a diagnostics GUI with a live yaw/pitch/roll graph, a Refresh button,
//     a one-click Repair Tracker button, axis controls, and Recenter (Ctrl+Alt+C);
//   * streams orientation over UDP as OpenTrack doubles plus a JSON datagram.
//
// This file is the entire bridge merged into one translation unit so it can be
// dropped into any project and published as a standalone open-source program.
// It contains no music-player, spatial-audio, or upmix code -- just the gyro /
// head-orientation pipeline.
//
// DEFAULT ORIENTATION CONVENTION
// ------------------------------
// The default axis convention is YXZ order with the X and Z axes inverted -- the
// mapping that produces correct head tracking on the WH-1000XM5. It is fully
// overridable: from the CLI with --axis-map / --invert, and live in the GUI with
// the axis-order dropdown and the Invert X/Y/Z checkboxes (Invert X and Z start
// checked).
//
// BUILD (Developer PowerShell / x64 Native Tools Command Prompt for VS):
//   rc /nologo app.rc
//   cl /std:c++latest /EHsc /permissive- /utf-8 /O2 /W4 ^
//      /DUNICODE /D_UNICODE xm5_head_tracker.cpp app.res /Fe:xm5-headtracker.exe
// (app.rc embeds the icon, version info, and the Common Controls v6 manifest.
//  All required import libraries are pulled in via #pragma comment below, so no
//  extra linker arguments are needed. Requires a C++20-capable MSVC and a current
//  Windows 11 SDK.)
//
// USAGE:
//   xm5-headtracker.exe                 (no args -> diagnostics GUI)
//   xm5-headtracker.exe probe [--include-disabled]
//   xm5-headtracker.exe dump [--seconds N]
//   xm5-headtracker.exe repair
//   xm5-headtracker.exe bluetooth-probe [--all-le] [--name FILTER]
//   xm5-headtracker.exe bluetooth-rebind [--name FILTER]   (default: auto-detect)
//   xm5-headtracker.exe bluetooth-generic-hid           (run elevated)
//   xm5-headtracker.exe bridge [--port 4242] [--seconds N]
//                              [--axis-map YXZ] [--invert XZ] [--smoothing 0.18]
//   xm5-headtracker.exe help | version
//
// The bridge sends six native little-endian doubles in OpenTrack pose order
// (x, y, z, yaw, pitch, roll) to the chosen UDP port, and a UTF-8 JSON object to
// port+1.
// UDP is loopback-only and unauthenticated; do not forward it to an untrusted
// network.
//
// -----------------------------------------------------------------------------
// MIT License
//
// Copyright (c) 2026 Nicholas Slattery and the XM5 Head Tracker Bridge contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// =============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// Winsock2 headers must precede Windows.h.
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <ws2bth.h>
#include <Windows.h>
#include <BluetoothAPIs.h>
#include <bluetoothleapis.h>
#include <bthledef.h>
#include <SetupAPI.h>
#include <newdev.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <SensorsApi.h>
#include <Sensors.h>
#include <PortableDeviceTypes.h>
#include <CommCtrl.h>
#include <Uxtheme.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <format>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "sensorsapi.lib")
#pragma comment(lib, "portabledeviceguids.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "BluetoothApis.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

// =============================================================================
//  Core types
// =============================================================================
namespace xm5 {

// Project version -- keep in sync with CHANGELOG.md and release tags.
inline constexpr std::wstring_view kVersion = L"1.2.0";

struct Vec3 {
    double x{};
    double y{};
    double z{};
    constexpr double& operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : z); }
    constexpr double operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

struct Quaternion {
    double w{1.0};
    double x{};
    double y{};
    double z{};
};

struct EulerDegrees {
    double yaw{};
    double pitch{};
    double roll{};
};

struct TrackingSample {
    Vec3 rotationVector{};
    Quaternion orientation{};
    EulerDegrees euler{};
    Vec3 angularVelocity{};       // gyroscope, radians/second
    Vec3 acceleration{};          // accelerometer, m/s^2 (only when the device reports it)
    bool hasAngularVelocity{};
    bool hasAcceleration{};
    std::uint8_t resetCounter{};
    double packetsPerSecond{};
    double receiveLatencyMs{-1.0};
    std::chrono::steady_clock::time_point receivedAt{};
    std::vector<std::uint8_t> rawReport;
};

struct AxisMapping {
    std::array<unsigned, 3> source{0, 1, 2};
    std::array<double, 3> sign{1.0, 1.0, 1.0};
};

struct FilterConfig {
    double smoothing{0.18};
    double fastMovementRadiansPerSecond{2.5};
    double driftCorrectionPerSecond{0.002};
    // Default convention: YXZ axis order with the X and Z axes inverted. This is
    // the mapping that yields correct head tracking on the WH-1000XM5. Overridable
    // via the CLI (--axis-map / --invert) and the GUI controls.
    AxisMapping axes{{1u, 0u, 2u}, {-1.0, 1.0, -1.0}};
};

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

} // namespace xm5

// =============================================================================
//  Logger
// =============================================================================
namespace xm5 {

enum class LogLevel { debug, info, warning, error };

class Logger {
public:
    using Sink = std::function<void(LogLevel, const std::wstring&)>;
    static Logger& instance();
    void setSink(Sink sink);
    void write(LogLevel level, std::wstring message);
    [[nodiscard]] std::vector<std::wstring> history() const;

private:
    mutable std::mutex mutex_;
    Sink sink_;
    std::vector<std::wstring> history_;
};

std::wstring windowsError(unsigned long code);

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setSink(Sink sink) {
    std::scoped_lock lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::write(LogLevel level, std::wstring message) {
    const auto now = std::chrono::system_clock::now();
    const wchar_t* label = level == LogLevel::error ? L"ERROR" : level == LogLevel::warning ? L"WARN" : level == LogLevel::debug ? L"DEBUG" : L"INFO";
    auto line = std::format(L"[{:%H:%M:%S}] {:5} {}", now, label, message);
    Sink sink;
    {
        std::scoped_lock lock(mutex_);
        history_.push_back(line);
        if (history_.size() > 2000) history_.erase(history_.begin(), history_.begin() + 500);
        sink = sink_;
    }
    OutputDebugStringW((line + L"\n").c_str());
    if (sink) sink(level, line);
}

std::vector<std::wstring> Logger::history() const {
    std::scoped_lock lock(mutex_);
    return history_;
}

std::wstring windowsError(unsigned long code) {
    wchar_t* buffer = nullptr;
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = length && buffer ? std::wstring(buffer, length) : std::format(L"Windows error {}", code);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}

} // namespace xm5

// =============================================================================
//  Math
// =============================================================================
namespace xm5 {

Quaternion normalize(Quaternion q);
Quaternion conjugate(Quaternion q);
Quaternion multiply(Quaternion a, Quaternion b);
Quaternion rotationVectorToQuaternion(Vec3 vector);
Vec3 quaternionToRotationVector(Quaternion q);
EulerDegrees quaternionToEulerDegrees(Quaternion q);
Quaternion slerp(Quaternion a, Quaternion b, double t);
Vec3 remap(Vec3 value, const AxisMapping& mapping);
double descriptorScale(std::int64_t raw, std::int32_t logicalMin, std::int32_t logicalMax,
                       std::int32_t physicalMin, std::int32_t physicalMax, std::int8_t unitExponent);
std::int8_t decodeHidUnitExponent(std::uint32_t exponent);
std::vector<double> decodePackedDescriptorValues(std::span<const std::uint8_t> packed, const DescriptorField& field);

Quaternion normalize(Quaternion q) {
    const auto n = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (n < 1e-12) return {};
    return {q.w/n, q.x/n, q.y/n, q.z/n};
}

Quaternion conjugate(Quaternion q) { return {q.w, -q.x, -q.y, -q.z}; }

Quaternion multiply(Quaternion a, Quaternion b) {
    return normalize({
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
    });
}

Quaternion rotationVectorToQuaternion(Vec3 v) {
    const auto angle = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (angle < 1e-12) return {};
    const auto s = std::sin(angle / 2.0) / angle;
    return normalize({std::cos(angle / 2.0), v.x*s, v.y*s, v.z*s});
}

Vec3 quaternionToRotationVector(Quaternion q) {
    q = normalize(q);
    if (q.w < 0.0) q = {-q.w, -q.x, -q.y, -q.z};
    const auto half = std::acos(std::clamp(q.w, -1.0, 1.0));
    const auto s = std::sin(half);
    if (std::abs(s) < 1e-12) return {};
    const auto factor = 2.0 * half / s;
    return {q.x*factor, q.y*factor, q.z*factor};
}

EulerDegrees quaternionToEulerDegrees(Quaternion q) {
    q = normalize(q);
    // Android head axes: X=right, Y=forward, Z=up. Yaw is around Z.
    const auto sinr = 2.0 * (q.w*q.x + q.y*q.z);
    const auto cosr = 1.0 - 2.0 * (q.x*q.x + q.y*q.y);
    const auto sinp = std::clamp(2.0 * (q.w*q.y - q.z*q.x), -1.0, 1.0);
    const auto siny = 2.0 * (q.w*q.z + q.x*q.y);
    const auto cosy = 1.0 - 2.0 * (q.y*q.y + q.z*q.z);
    constexpr auto d = 180.0 / std::numbers::pi;
    return {std::atan2(siny, cosy)*d, std::asin(sinp)*d, std::atan2(sinr, cosr)*d};
}

Quaternion slerp(Quaternion a, Quaternion b, double t) {
    a = normalize(a); b = normalize(b); t = std::clamp(t, 0.0, 1.0);
    auto dot = a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
    if (dot < 0.0) { b = {-b.w, -b.x, -b.y, -b.z}; dot = -dot; }
    if (dot > 0.9995) return normalize({a.w+t*(b.w-a.w), a.x+t*(b.x-a.x), a.y+t*(b.y-a.y), a.z+t*(b.z-a.z)});
    const auto theta = std::acos(std::clamp(dot, -1.0, 1.0));
    const auto scale = std::sin(theta);
    const auto aa = std::sin((1.0-t)*theta)/scale;
    const auto bb = std::sin(t*theta)/scale;
    return normalize({aa*a.w+bb*b.w, aa*a.x+bb*b.x, aa*a.y+bb*b.y, aa*a.z+bb*b.z});
}

Vec3 remap(Vec3 v, const AxisMapping& m) {
    return {v[m.source[0]]*m.sign[0], v[m.source[1]]*m.sign[1], v[m.source[2]]*m.sign[2]};
}

double descriptorScale(std::int64_t raw, std::int32_t lmin, std::int32_t lmax,
                       std::int32_t pmin, std::int32_t pmax, std::int8_t exponent) {
    if (lmax == lmin || (pmax == 0 && pmin == 0)) return static_cast<double>(raw);
    const auto fraction = (static_cast<double>(raw) - lmin) / (static_cast<double>(lmax) - lmin);
    return (pmin + fraction * (static_cast<double>(pmax) - pmin)) * std::pow(10.0, exponent);
}

std::int8_t decodeHidUnitExponent(std::uint32_t exponent) {
    auto nibble = static_cast<std::int8_t>(exponent & 0x0f);
    return nibble >= 8 ? static_cast<std::int8_t>(nibble - 16) : nibble;
}

std::vector<double> decodePackedDescriptorValues(std::span<const std::uint8_t> packed, const DescriptorField& field) {
    std::vector<double> result;
    if (!field.bitSize || field.bitSize > 63) return result;
    result.reserve(field.reportCount);
    for (unsigned valueIndex=0; valueIndex<field.reportCount; ++valueIndex) {
        std::uint64_t raw{};
        const auto offset=static_cast<std::size_t>(valueIndex)*field.bitSize;
        for (unsigned bitIndex=0; bitIndex<field.bitSize; ++bitIndex) {
            const auto bit=offset+bitIndex;
            if (bit/8 < packed.size() && (packed[bit/8] & (1u << (bit%8)))) raw |= std::uint64_t{1} << bitIndex;
        }
        std::int64_t value=static_cast<std::int64_t>(raw);
        if (field.logicalMin < 0) {
            const auto sign=std::uint64_t{1} << (field.bitSize-1);
            const auto mask=(std::uint64_t{1} << field.bitSize)-1;
            value=static_cast<std::int64_t>(((raw&mask)^sign)-sign);
        }
        result.push_back(descriptorScale(value,field.logicalMin,field.logicalMax,field.physicalMin,field.physicalMax,field.unitExponent));
    }
    return result;
}

} // namespace xm5

// =============================================================================
//  Raw HID backend
// =============================================================================
namespace xm5 {

// Bluetooth helpers, defined in the Bluetooth section below. The HID backend
// uses them to resolve which paired headset a head-tracker HID node belongs to.
std::wstring bluetoothNameForHidInstance(std::wstring_view instanceId);
std::vector<std::wstring> pairedBluetoothDeviceNames();

class HidBackend {
public:
    using RawCallback = std::function<void(const std::vector<std::uint8_t>&)>;
    using SampleCallback = std::function<void(TrackingSample)>;

    HidBackend();
    ~HidBackend();
    std::vector<DeviceInfo> enumerate(bool presentInterfacesOnly = true);
    bool connect(const DeviceInfo& device, RawCallback raw, SampleCallback sample);
    void disconnect();
    [[nodiscard]] bool connected() const { return running_; }

private:
    struct Context;
    std::unique_ptr<Context> context_;
    std::jthread reader_;
    std::atomic_bool running_{};
};

std::wstring hexDump(const std::vector<std::uint8_t>& bytes);

namespace {

constexpr USAGE kSensorPage = 0x20;
constexpr USAGE kOtherCustom = 0xE1;
constexpr USAGE kSensorDescription = 0x0308;
constexpr USAGE kReportInterval = 0x030E;
constexpr USAGE kReportingAllEvents = 0x0841;
constexpr USAGE kPowerFull = 0x0851;
constexpr USAGE kTransportAcl = 0xF800;
// Android Head Tracker custom data fields (HID sensor page 0x20).
constexpr USAGE kRotation = 0x0544;        // orientation rotation vector
constexpr USAGE kAngularVelocity = 0x0545; // gyroscope (rad/s), vector form
constexpr USAGE kResetCounter = 0x0546;
// Standard HID sensor-page motion fields, parsed opportunistically so that any
// firmware which also reports raw inertial data has it surfaced. The XM5's
// Android Head Tracker profile normally exposes orientation + gyro only.
constexpr USAGE kAccelerationVector = 0x0452; // acceleration, vector form
constexpr USAGE kAccelerationX = 0x0453;      // acceleration about X (m/s^2)
constexpr USAGE kAccelerationY = 0x0454;
constexpr USAGE kAccelerationZ = 0x0455;
constexpr USAGE kAngularVelocityVector = 0x0456; // angular velocity, vector form
constexpr USAGE kAngularVelocityX = 0x0457;      // angular velocity about X (rad/s)
constexpr USAGE kAngularVelocityY = 0x0458;
constexpr USAGE kAngularVelocityZ = 0x0459;
constexpr std::string_view kMarker = "#AndroidHeadTracker#";

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = std::exchange(other.value, INVALID_HANDLE_VALUE); } return *this; }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
};

struct Preparsed {
    PHIDP_PREPARSED_DATA value{};
    ~Preparsed() { if (value) HidD_FreePreparsedData(value); }
    Preparsed() = default;
    Preparsed(Preparsed&& o) noexcept : value(std::exchange(o.value, nullptr)) {}
    Preparsed& operator=(Preparsed&& o) noexcept { if (this != &o) { if(value) HidD_FreePreparsedData(value); value=std::exchange(o.value,nullptr); } return *this; }
    Preparsed(const Preparsed&) = delete; Preparsed& operator=(const Preparsed&) = delete;
};

std::wstring hidString(HANDLE h, BOOLEAN (__stdcall *fn)(HANDLE, PVOID, ULONG)) {
    std::array<wchar_t, 256> b{}; return fn(h, b.data(), static_cast<ULONG>(b.size()*sizeof(wchar_t))) ? b.data() : L"";
}

DescriptorField makeField(const HIDP_VALUE_CAPS& c, bool feature) {
    DescriptorField f;
    f.usagePage=c.UsagePage; f.usage=c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
    f.reportId=c.ReportID; f.reportCount=c.ReportCount; f.bitSize=c.BitSize;
    f.logicalMin=c.LogicalMin; f.logicalMax=c.LogicalMax; f.physicalMin=c.PhysicalMin; f.physicalMax=c.PhysicalMax;
    f.unitExponent=decodeHidUnitExponent(c.UnitsExp); f.unit=c.Units;
    f.dataIndex=c.IsRange ? c.Range.DataIndexMin : c.NotRange.DataIndex; f.feature=feature;
    return f;
}

std::vector<HIDP_VALUE_CAPS> getValueCaps(HIDP_REPORT_TYPE type, PHIDP_PREPARSED_DATA ppd, USHORT count) {
    std::vector<HIDP_VALUE_CAPS> result(count);
    if (!count) return result;
    auto n=count; if (HidP_GetValueCaps(type, result.data(), &n, ppd) != HIDP_STATUS_SUCCESS) return {};
    result.resize(n); return result;
}

std::vector<HIDP_BUTTON_CAPS> getButtonCaps(HIDP_REPORT_TYPE type, PHIDP_PREPARSED_DATA ppd, USHORT count) {
    std::vector<HIDP_BUTTON_CAPS> result(count);
    if (!count) return result;
    auto n=count; if (HidP_GetButtonCaps(type, result.data(), &n, ppd) != HIDP_STATUS_SUCCESS) return {};
    result.resize(n); return result;
}

std::string extractDescription(HANDLE handle, PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps,
                               const std::vector<HIDP_VALUE_CAPS>& featureCaps, std::vector<std::string>& diagnostics) {
    for (const auto& c : featureCaps) {
        const auto usage = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
        if (c.UsagePage != kSensorPage || usage != kSensorDescription) continue;
        std::vector<std::uint8_t> report(caps.FeatureReportByteLength); report[0]=c.ReportID;
        if (!HidD_GetFeature(handle, report.data(), static_cast<ULONG>(report.size()))) {
            diagnostics.push_back(std::format("feature report {} read failed: {}", c.ReportID, GetLastError())); continue;
        }
        std::ostringstream raw; raw<<"feature report "<<static_cast<unsigned>(c.ReportID)<<":"<<std::hex<<std::setfill('0');
        for(const auto b:report)raw<<' '<<std::setw(2)<<static_cast<unsigned>(b);diagnostics.push_back(raw.str());
        const auto byteCount = static_cast<USHORT>((static_cast<unsigned long long>(c.ReportCount)*c.BitSize+7)/8);
        std::vector<std::uint8_t> value(byteCount);
        auto status = HidP_GetUsageValueArray(HidP_Feature, c.UsagePage, c.LinkCollection, usage,
            reinterpret_cast<PCHAR>(value.data()), byteCount, ppd, reinterpret_cast<PCHAR>(report.data()), static_cast<ULONG>(report.size()));
        if (status == HIDP_STATUS_SUCCESS) {
            std::string s(value.begin(), value.end());
            while (!s.empty() && s.back()=='\0') s.pop_back();
            return s;
        }
        // Constant sensor-description fields are not exposed by some Windows HID parser versions.
        const auto it = std::search(report.begin(), report.end(), kMarker.begin(), kMarker.end());
        if (it != report.end()) {
            std::string s(it, report.end()); while (!s.empty() && (s.back()=='\0' || static_cast<unsigned char>(s.back())==0xff)) s.pop_back(); return s;
        }
    }
    // Some Sensor HID class stacks omit constant fields from value capabilities. Probe only report IDs
    // discovered from the descriptor's remaining feature capabilities, never guessed numeric IDs.
    std::set<std::uint8_t> reportIds;
    for (const auto& c : featureCaps) reportIds.insert(c.ReportID);
    for (const auto reportId : reportIds) {
        std::vector<std::uint8_t> report(caps.FeatureReportByteLength); report[0]=reportId;
        if (!HidD_GetFeature(handle,report.data(),static_cast<ULONG>(report.size()))) continue;
        std::ostringstream dump; dump<<"feature report "<<static_cast<unsigned>(reportId)<<":";
        dump<<std::hex<<std::setfill('0'); for(const auto b:report) dump<<' '<<std::setw(2)<<static_cast<unsigned>(b);
        diagnostics.push_back(dump.str());
        const auto it=std::search(report.begin(),report.end(),kMarker.begin(),kMarker.end());
        if(it!=report.end()){std::string s(it,report.end());while(!s.empty()&&(s.back()=='\0'||static_cast<unsigned char>(s.back())==0xff))s.pop_back();return s;}
    }
    return {};
}

bool updateArrayFeature(HANDLE handle, PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps,
                        const std::vector<HIDP_BUTTON_CAPS>& buttons, USAGE desired, std::wstring_view label, bool warnIfMissing=true) {
    for (const auto& b : buttons) {
        const auto min = b.IsRange ? b.Range.UsageMin : b.NotRange.Usage;
        const auto max = b.IsRange ? b.Range.UsageMax : b.NotRange.Usage;
        if (b.UsagePage != kSensorPage || desired < min || desired > max) continue;
        std::vector<std::uint8_t> report(caps.FeatureReportByteLength); report[0]=b.ReportID;
        HidD_GetFeature(handle, report.data(), static_cast<ULONG>(report.size()));
        const auto maximum=HidP_MaxUsageListLength(HidP_Feature,b.UsagePage,ppd);
        if(maximum){std::vector<USAGE> existing(maximum);ULONG existingCount=maximum;if(HidP_GetUsages(HidP_Feature,b.UsagePage,b.LinkCollection,existing.data(),&existingCount,ppd,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()))==HIDP_STATUS_SUCCESS&&existingCount)HidP_UnsetUsages(HidP_Feature,b.UsagePage,b.LinkCollection,existing.data(),&existingCount,ppd,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()));}
        ULONG count=1; USAGE usage=desired;
        const auto parsed = HidP_SetUsages(HidP_Feature, b.UsagePage, b.LinkCollection, &usage, &count, ppd,
            reinterpret_cast<PCHAR>(report.data()), static_cast<ULONG>(report.size()));
        if (parsed != HIDP_STATUS_SUCCESS || !HidD_SetFeature(handle, report.data(), static_cast<ULONG>(report.size()))) {
            Logger::instance().write(LogLevel::error, std::format(L"Failed to set {} (parser=0x{:08X}, Win32={}: {})", label, static_cast<unsigned>(parsed), GetLastError(), windowsError(GetLastError())));
            return false;
        }
        Logger::instance().write(LogLevel::info, std::format(L"Set {} using descriptor report ID {}", label, b.ReportID)); return true;
    }
    if(warnIfMissing)Logger::instance().write(LogLevel::warning, std::format(L"Descriptor does not expose writable {} selector", label)); return false;
}

bool updateInterval(HANDLE handle, PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps, const std::vector<HIDP_VALUE_CAPS>& values) {
    for (const auto& c : values) {
        const auto usage=c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
        if (c.UsagePage != kSensorPage || usage != kReportInterval) continue;
        const auto low=std::min(c.PhysicalMin,c.PhysicalMax), high=std::max(c.PhysicalMin,c.PhysicalMax);
        const auto exponent=decodeHidUnitExponent(c.UnitsExp);
        const auto unitScale=std::pow(10.0,exponent);
        const auto supportedLow=low*unitScale,supportedHigh=high*unitScale;
        auto targetSeconds=std::max(0.010,supportedLow);
        if(targetSeconds>0.020||supportedHigh<0.010){targetSeconds=supportedLow;Logger::instance().write(LogLevel::warning,std::format(L"Device interval range {:.3f}..{:.3f} ms does not support the protocol's 10..20 ms target; using fastest advertised interval {:.3f} ms",supportedLow*1000.0,supportedHigh*1000.0,targetSeconds*1000.0));}
        const LONG target=std::clamp<LONG>(static_cast<LONG>(std::llround(targetSeconds/unitScale)),low,high);
        std::vector<std::uint8_t> report(caps.FeatureReportByteLength); report[0]=c.ReportID;
        HidD_GetFeature(handle, report.data(), static_cast<ULONG>(report.size()));
        const auto status=HidP_SetScaledUsageValue(HidP_Feature,c.UsagePage,c.LinkCollection,usage,target,ppd,
            reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()));
        if(status != HIDP_STATUS_SUCCESS || !HidD_SetFeature(handle,report.data(),static_cast<ULONG>(report.size()))) {
            Logger::instance().write(LogLevel::error,std::format(L"Failed setting report interval (parser=0x{:08X}, Win32={})",static_cast<unsigned>(status),GetLastError())); return false;
        }
        Logger::instance().write(LogLevel::info,std::format(L"Set report interval to {} x 10^{} seconds (report ID {})",target,exponent,c.ReportID)); return true;
    }
    Logger::instance().write(LogLevel::warning,L"Descriptor does not expose writable report interval"); return false;
}

bool configureHeadTrackerFeatures(HANDLE handle,PHIDP_PREPARSED_DATA ppd,const HIDP_CAPS& caps,const std::vector<HIDP_VALUE_CAPS>& values,const std::vector<HIDP_BUTTON_CAPS>& buttons){
    std::map<UCHAR,std::vector<std::uint8_t>> reports;auto ensure=[&](UCHAR id)->std::vector<std::uint8_t>&{auto& report=reports[id];if(report.empty()){report.resize(caps.FeatureReportByteLength);report[0]=id;}return report;};
    for(const auto& c:values){const auto usage=c.IsRange?c.Range.UsageMin:c.NotRange.Usage;if(c.UsagePage!=kSensorPage||usage!=kReportInterval)continue;auto& report=ensure(c.ReportID);const auto low=std::min(c.PhysicalMin,c.PhysicalMax),high=std::max(c.PhysicalMin,c.PhysicalMax);const auto exponent=decodeHidUnitExponent(c.UnitsExp);const auto scale=std::pow(10.0,exponent);const auto supportedLow=low*scale,supportedHigh=high*scale;auto targetSeconds=std::max(0.010,supportedLow);if(targetSeconds>0.020||supportedHigh<0.010){targetSeconds=supportedLow;Logger::instance().write(LogLevel::warning,std::format(L"Device interval range {:.3f}..{:.3f} ms is outside 10..20 ms; using {:.3f} ms",supportedLow*1000.0,supportedHigh*1000.0,targetSeconds*1000.0));}const auto target=std::clamp<LONG>(static_cast<LONG>(std::llround(targetSeconds/scale)),low,high);const auto status=HidP_SetScaledUsageValue(HidP_Feature,c.UsagePage,c.LinkCollection,usage,target,ppd,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()));if(status!=HIDP_STATUS_SUCCESS){Logger::instance().write(LogLevel::error,std::format(L"Could not encode report interval (0x{:08X})",static_cast<unsigned>(status)));return false;}Logger::instance().write(LogLevel::info,std::format(L"Encoded interval {} x 10^{} seconds in report {}",target,exponent,c.ReportID));}
    const std::array<std::pair<USAGE,std::wstring_view>,3> desired{{{kTransportAcl,L"v2 ACL transport"},{kPowerFull,L"Full Power"},{kReportingAllEvents,L"All Events reporting"}}};
    for(const auto& [usage,label]:desired){bool exposed{};for(const auto& b:buttons){const auto min=b.IsRange?b.Range.UsageMin:b.NotRange.Usage,max=b.IsRange?b.Range.UsageMax:b.NotRange.Usage;if(b.UsagePage!=kSensorPage||usage<min||usage>max)continue;exposed=true;auto& report=ensure(b.ReportID);ULONG count=1;auto mutableUsage=usage;const auto status=HidP_SetUsages(HidP_Feature,b.UsagePage,b.LinkCollection,&mutableUsage,&count,ppd,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()));if(status!=HIDP_STATUS_SUCCESS){Logger::instance().write(LogLevel::error,std::format(L"Could not encode {} (0x{:08X})",label,static_cast<unsigned>(status)));return false;}Logger::instance().write(LogLevel::info,std::format(L"Encoded {} in report {}",label,b.ReportID));break;}if(!exposed&&usage!=kTransportAcl){Logger::instance().write(LogLevel::error,std::format(L"Descriptor lacks {}",label));return false;}}
    for(auto& [id,report]:reports){Logger::instance().write(LogLevel::info,std::format(L"Sending combined feature report {} ({} bytes)",id,report.size()));if(!HidD_SetFeature(handle,report.data(),static_cast<ULONG>(report.size()))){Logger::instance().write(LogLevel::error,std::format(L"SetFeature report {} failed: {}",id,windowsError(GetLastError())));return false;}Logger::instance().write(LogLevel::info,std::format(L"Feature report {} accepted",id));}return !reports.empty();
}

std::vector<double> usageArray(PHIDP_PREPARSED_DATA ppd, const HIDP_VALUE_CAPS& c, const std::vector<std::uint8_t>& report) {
    const auto usage=c.IsRange?c.Range.UsageMin:c.NotRange.Usage;
    const auto bytesCount=static_cast<USHORT>((static_cast<unsigned long long>(c.ReportCount)*c.BitSize+7)/8);
    std::vector<std::uint8_t> packed(bytesCount);
    if(HidP_GetUsageValueArray(HidP_Input,c.UsagePage,c.LinkCollection,usage,reinterpret_cast<PCHAR>(packed.data()),bytesCount,ppd,
        reinterpret_cast<PCHAR>(const_cast<std::uint8_t*>(report.data())),static_cast<ULONG>(report.size())) != HIDP_STATUS_SUCCESS) return {};
    return decodePackedDescriptorValues(packed,makeField(c,false));
}

// Reads a single scaled scalar value, honouring the descriptor's logical/physical
// ranges and unit exponent. Used for the per-axis acceleration / gyro usages.
bool scalarValue(PHIDP_PREPARSED_DATA ppd, const HIDP_VALUE_CAPS& c, const std::vector<std::uint8_t>& report, double& out) {
    const auto usage=c.IsRange?c.Range.UsageMin:c.NotRange.Usage;
    LONG scaled{};
    if(HidP_GetScaledUsageValue(HidP_Input,c.UsagePage,c.LinkCollection,usage,&scaled,ppd,
        reinterpret_cast<PCHAR>(const_cast<std::uint8_t*>(report.data())),static_cast<ULONG>(report.size()))==HIDP_STATUS_SUCCESS){out=static_cast<double>(scaled);return true;}
    ULONG raw{};
    if(HidP_GetUsageValue(HidP_Input,c.UsagePage,c.LinkCollection,usage,&raw,ppd,
        reinterpret_cast<PCHAR>(const_cast<std::uint8_t*>(report.data())),static_cast<ULONG>(report.size()))!=HIDP_STATUS_SUCCESS) return false;
    std::int64_t value=static_cast<std::int64_t>(raw);
    if(c.LogicalMin<0&&c.BitSize&&c.BitSize<64){const auto sign=std::uint64_t{1}<<(c.BitSize-1);const auto mask=(std::uint64_t{1}<<c.BitSize)-1;value=static_cast<std::int64_t>(((static_cast<std::uint64_t>(raw)&mask)^sign)-sign);}
    out=descriptorScale(value,c.LogicalMin,c.LogicalMax,c.PhysicalMin,c.PhysicalMax,decodeHidUnitExponent(c.UnitsExp));
    return true;
}

} // namespace

struct HidBackend::Context {
    Handle handle;
    Preparsed ppd;
    HIDP_CAPS caps{};
    std::vector<HIDP_VALUE_CAPS> inputValues;
    RawCallback raw;
    SampleCallback sample;
    std::chrono::steady_clock::time_point rateStart{std::chrono::steady_clock::now()};
    std::uint64_t rateCount{};
    double rate{};
};

HidBackend::HidBackend() = default;
HidBackend::~HidBackend() { disconnect(); }

std::vector<DeviceInfo> HidBackend::enumerate(bool presentInterfacesOnly) {
    std::vector<DeviceInfo> devices;
    GUID guid{}; HidD_GetHidGuid(&guid);
    const auto flags=DIGCF_DEVICEINTERFACE|(presentInterfacesOnly?DIGCF_PRESENT:0);
    const auto set=SetupDiGetClassDevsW(&guid,nullptr,nullptr,flags);
    if(set==INVALID_HANDLE_VALUE) { Logger::instance().write(LogLevel::error,std::format(L"SetupAPI HID enumeration failed: {}",windowsError(GetLastError()))); return devices; }
    SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize=sizeof(iface);
    for(DWORD index=0;SetupDiEnumDeviceInterfaces(set,nullptr,&guid,index,&iface);++index) {
        DWORD needed{}; SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&needed,nullptr);
        std::vector<std::uint8_t> storage(needed); auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data()); detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA dev{}; dev.cbSize=sizeof(dev);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,needed,nullptr,&dev)) continue;
        DeviceInfo info; info.path=detail->DevicePath;
        wchar_t instance[MAX_DEVICE_ID_LEN]{}; if(CM_Get_Device_IDW(dev.DevInst,instance,MAX_DEVICE_ID_LEN,0)==CR_SUCCESS) info.instanceId=instance;
        Handle handle(CreateFileW(info.path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
        if(handle.value==INVALID_HANDLE_VALUE) {
            const auto writeError=GetLastError(); handle=Handle(CreateFileW(info.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
            if(handle.value==INVALID_HANDLE_VALUE) handle=Handle(CreateFileW(info.path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
            if(handle.value==INVALID_HANDLE_VALUE) {
                info.accessDenied=writeError==ERROR_ACCESS_DENIED||GetLastError()==ERROR_ACCESS_DENIED;
                Logger::instance().write(LogLevel::error,std::format(L"Cannot open HID {}: {}",info.instanceId,windowsError(GetLastError()))); devices.push_back(std::move(info)); continue;
            }
            Logger::instance().write(LogLevel::warning,std::format(L"HID {} is not writable: {}",info.instanceId,windowsError(writeError)));
        }
        HIDD_ATTRIBUTES attributes{}; attributes.Size=sizeof(attributes); if(HidD_GetAttributes(handle.value,&attributes)) { info.vendorId=attributes.VendorID;info.productId=attributes.ProductID;info.version=attributes.VersionNumber; }
        info.product=hidString(handle.value,HidD_GetProductString); info.manufacturer=hidString(handle.value,HidD_GetManufacturerString);
        Preparsed ppd; if(!HidD_GetPreparsedData(handle.value,&ppd.value)) { devices.push_back(std::move(info)); continue; }
        HIDP_CAPS caps{}; if(HidP_GetCaps(ppd.value,&caps)!=HIDP_STATUS_SUCCESS) { devices.push_back(std::move(info)); continue; }
        info.usagePage=caps.UsagePage;info.usage=caps.Usage;info.inputReportBytes=caps.InputReportByteLength;info.featureReportBytes=caps.FeatureReportByteLength;
        auto inputs=getValueCaps(HidP_Input,ppd.value,caps.NumberInputValueCaps);
        auto features=getValueCaps(HidP_Feature,ppd.value,caps.NumberFeatureValueCaps);
        for(const auto& c:inputs) info.fields.push_back(makeField(c,false));
        for(const auto& c:features) info.fields.push_back(makeField(c,true));
        if(info.usagePage==kSensorPage&&info.usage==kOtherCustom) {
            info.sensorDescription=extractDescription(handle.value,ppd.value,caps,features,info.featureValues);
            info.androidHeadTracker=info.sensorDescription.starts_with(kMarker);
            info.bluetoothName=bluetoothNameForHidInstance(info.instanceId);
            Logger::instance().write(info.androidHeadTracker?LogLevel::info:LogLevel::warning,
                std::format(L"Candidate HID VID={:04X} PID={:04X}, headset='{}', description='{}'",info.vendorId,info.productId,
                    info.bluetoothName.empty()?L"(unresolved)":info.bluetoothName,
                    std::wstring(info.sensorDescription.begin(),info.sensorDescription.end())));
        }
        devices.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(set);
    Logger::instance().write(LogLevel::info,std::format(L"SetupAPI discovered {} HID top-level collection(s)",devices.size()));
    return devices;
}

bool HidBackend::connect(const DeviceInfo& device, RawCallback raw, SampleCallback sample) {
    disconnect(); auto ctx=std::make_unique<Context>();
    ctx->handle=Handle(CreateFileW(device.path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    if(ctx->handle.value==INVALID_HANDLE_VALUE) { Logger::instance().write(LogLevel::error,std::format(L"Head tracker open failed: {}",windowsError(GetLastError()))); return false; }
    if(!HidD_GetPreparsedData(ctx->handle.value,&ctx->ppd.value)||HidP_GetCaps(ctx->ppd.value,&ctx->caps)!=HIDP_STATUS_SUCCESS) {
        Logger::instance().write(LogLevel::error,L"Could not obtain head tracker preparsed descriptor"); return false;
    }
    ctx->inputValues=getValueCaps(HidP_Input,ctx->ppd.value,ctx->caps.NumberInputValueCaps);
    const auto featureValues=getValueCaps(HidP_Feature,ctx->ppd.value,ctx->caps.NumberFeatureValueCaps);
    const auto featureButtons=getButtonCaps(HidP_Feature,ctx->ppd.value,ctx->caps.NumberFeatureButtonCaps);
    if(!configureHeadTrackerFeatures(ctx->handle.value,ctx->ppd.value,ctx->caps,featureValues,featureButtons)){Logger::instance().write(LogLevel::error,L"Head tracker feature configuration failed");return false;}
    ctx->raw=std::move(raw);ctx->sample=std::move(sample);context_=std::move(ctx);running_=true;
    reader_=std::jthread([this](std::stop_token stop) {
        auto* c=context_.get(); std::vector<std::uint8_t> report(c->caps.InputReportByteLength);
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        while(!stop.stop_requested()&&running_) {
            OVERLAPPED ov{};ov.hEvent=event.value;ResetEvent(event.value);DWORD bytes{};
            if(!ReadFile(c->handle.value,report.data(),static_cast<DWORD>(report.size()),&bytes,&ov)&&GetLastError()!=ERROR_IO_PENDING) {
                Logger::instance().write(LogLevel::error,std::format(L"HID read failed: {}",windowsError(GetLastError())));break;
            }
            while(!stop.stop_requested()) { const auto wait=WaitForSingleObject(event.value,100);if(wait==WAIT_OBJECT_0)break;if(wait==WAIT_FAILED)break; }
            if(stop.stop_requested()) { CancelIoEx(c->handle.value,&ov);break; }
            if(!GetOverlappedResult(c->handle.value,&ov,&bytes,FALSE)) { Logger::instance().write(LogLevel::error,std::format(L"HID asynchronous read failed: {}",windowsError(GetLastError())));break; }
            if(bytes==0)continue;report.resize(bytes);if(c->raw)c->raw(report);
            TrackingSample s;s.rawReport=report;s.receivedAt=std::chrono::steady_clock::now();bool gotRotation=false;
            for(const auto& field:c->inputValues) {
                if(field.ReportID && report[0]!=field.ReportID)continue;
                const auto usage=field.IsRange?field.Range.UsageMin:field.NotRange.Usage;if(field.UsagePage!=kSensorPage)continue;
                if(usage==kRotation||usage==kAngularVelocity||usage==kAngularVelocityVector||usage==kAccelerationVector) {
                    // Vector-form fields: a packed array of three values.
                    const auto values=usageArray(c->ppd.value,field,report);if(values.size()<3)continue;
                    if(usage==kRotation){s.rotationVector={values[0],values[1],values[2]};gotRotation=true;}
                    else if(usage==kAccelerationVector){s.acceleration={values[0],values[1],values[2]};s.hasAcceleration=true;}
                    else{s.angularVelocity={values[0],values[1],values[2]};s.hasAngularVelocity=true;} // 0x0545 / 0x0456
                } else if(usage==kAccelerationX||usage==kAccelerationY||usage==kAccelerationZ) {
                    double v{};if(scalarValue(c->ppd.value,field,report,v)){s.acceleration[usage-kAccelerationX]=v;s.hasAcceleration=true;}
                } else if(usage==kAngularVelocityX||usage==kAngularVelocityY||usage==kAngularVelocityZ) {
                    double v{};if(scalarValue(c->ppd.value,field,report,v)){s.angularVelocity[usage-kAngularVelocityX]=v;s.hasAngularVelocity=true;}
                } else if(usage==kResetCounter) {
                    ULONG v{};if(HidP_GetUsageValue(HidP_Input,field.UsagePage,field.LinkCollection,usage,&v,c->ppd.value,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()))==HIDP_STATUS_SUCCESS)s.resetCounter=static_cast<std::uint8_t>(v);
                }
            }
            ++c->rateCount;const auto elapsed=std::chrono::duration<double>(s.receivedAt-c->rateStart).count();if(elapsed>=1.0){c->rate=c->rateCount/elapsed;c->rateCount=0;c->rateStart=s.receivedAt;}s.packetsPerSecond=c->rate;s.receiveLatencyMs=-1.0;
            if(gotRotation&&c->sample)c->sample(std::move(s));
            report.resize(c->caps.InputReportByteLength);
        }
        running_=false;
    });
    Logger::instance().write(LogLevel::info,L"Asynchronous HID report reader started");return true;
}

void HidBackend::disconnect() {
    running_=false;if(reader_.joinable()){reader_.request_stop();if(context_&&context_->handle.value!=INVALID_HANDLE_VALUE)CancelIoEx(context_->handle.value,nullptr);reader_.join();}context_.reset();
}

std::wstring hexDump(const std::vector<std::uint8_t>& bytes) {
    std::wostringstream out;out<<std::hex<<std::uppercase<<std::setfill(L'0');for(std::size_t i=0;i<bytes.size();++i){if(i)out<<L' ';out<<std::setw(2)<<static_cast<unsigned>(bytes[i]);}return out.str();
}

} // namespace xm5

// =============================================================================
//  Windows Sensor API fallback backend
// =============================================================================
namespace xm5 {

class SensorBackend {
public:
    using SampleCallback = std::function<void(TrackingSample)>;
    ~SensorBackend();
    std::vector<SensorInfo> enumerate();
    bool connect(const SensorInfo& sensor, SampleCallback sample);
    void disconnect();
    [[nodiscard]] bool connected() const { return running_; }

private:
    std::jthread reader_;
    std::atomic_bool running_{};
};

namespace {
std::wstring getString(ISensor* sensor, REFPROPERTYKEY key) {
    PROPVARIANT value{}; PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(sensor->GetProperty(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) result = value.pwszVal;
    PropVariantClear(&value); return result;
}
std::wstring guidString(REFGUID guid) { wchar_t b[40]{}; StringFromGUID2(guid, b, 40); return b; }
std::vector<double> numbers(const PROPVARIANT& v) {
    if(v.vt==VT_R4)return {v.fltVal};if(v.vt==VT_R8)return {v.dblVal};if(v.vt==VT_UI1)return {static_cast<double>(v.bVal)};if(v.vt==VT_UI4)return {static_cast<double>(v.ulVal)};
    if(v.vt==(VT_VECTOR|VT_R4))return std::vector<double>(v.caflt.pElems,v.caflt.pElems+v.caflt.cElems);
    if(v.vt==(VT_VECTOR|VT_R8))return std::vector<double>(v.cadbl.pElems,v.cadbl.pElems+v.cadbl.cElems);
    if(v.vt==(VT_VECTOR|VT_UI4)){std::vector<double> r;for(ULONG i=0;i<v.caul.cElems;++i)r.push_back(v.caul.pElems[i]);return r;}return {};
}
std::vector<double> getNumbers(ISensorDataReport* report,REFPROPERTYKEY key) { PROPVARIANT v{};PropVariantInit(&v);std::vector<double> r;if(SUCCEEDED(report->GetSensorValue(key,&v)))r=numbers(v);PropVariantClear(&v);return r; }
double reportLatencyMs(ISensorDataReport* report) {
    SYSTEMTIME stamp{};if(FAILED(report->GetTimestamp(&stamp)))return -1.0;FILETIME ft{},now{};if(!SystemTimeToFileTime(&stamp,&ft))return -1.0;GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{{ft.dwLowDateTime,ft.dwHighDateTime}},b{{now.dwLowDateTime,now.dwHighDateTime}};if(b.QuadPart<a.QuadPart)return -1.0;return (b.QuadPart-a.QuadPart)/10000.0;
}
}

std::vector<SensorInfo> SensorBackend::enumerate() {
    std::vector<SensorInfo> result;
    const auto init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<ISensorManager> manager;
    auto hr = CoCreateInstance(CLSID_SensorManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(hr)) {
        Logger::instance().write(LogLevel::warning, std::format(L"Windows Sensor API unavailable (0x{:08X})", static_cast<unsigned>(hr)));
        if (SUCCEEDED(init)) CoUninitialize(); return result;
    }
    ComPtr<ISensorCollection> sensors;
    hr = manager->GetSensorsByCategory(SENSOR_CATEGORY_OTHER, &sensors);
    if (FAILED(hr)) hr = manager->GetSensorsByType(SENSOR_TYPE_CUSTOM, &sensors);
    ULONG count{}; if (sensors) sensors->GetCount(&count);
    Logger::instance().write(LogLevel::info, std::format(L"Sensor API discovered {} custom/other sensor(s)", count));
    for (ULONG i=0; i<count; ++i) {
        ComPtr<ISensor> sensor; if (FAILED(sensors->GetAt(i, &sensor))) continue;
        GUID id{}, type{}; sensor->GetID(&id); sensor->GetType(&type);
        SensorInfo info;
        info.friendlyName = getString(sensor.Get(), SENSOR_PROPERTY_FRIENDLY_NAME);
        info.description = getString(sensor.Get(), SENSOR_PROPERTY_DESCRIPTION);
        info.id = guidString(id); info.type = guidString(type);
        const auto marker = std::wstring(L"#AndroidHeadTracker#");
        info.androidHeadTracker = info.description.starts_with(marker) || info.friendlyName.find(marker) != std::wstring::npos;
        result.push_back(std::move(info));
    }
    sensors.Reset();manager.Reset();
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

SensorBackend::~SensorBackend(){disconnect();}

bool SensorBackend::connect(const SensorInfo& info,SampleCallback callback) {
    disconnect();GUID sensorId{};if(FAILED(CLSIDFromString(info.id.c_str(),&sensorId)))return false;running_=true;
    reader_=std::jthread([this,sensorId,callback=std::move(callback)](std::stop_token stop){
        const auto init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);ComPtr<ISensorManager> manager;ComPtr<ISensor> sensor;
        auto hr=CoCreateInstance(CLSID_SensorManager,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&manager));if(SUCCEEDED(hr))hr=manager->GetSensorByID(sensorId,&sensor);
        if(FAILED(hr)){Logger::instance().write(LogLevel::error,std::format(L"Sensor API open failed (0x{:08X}); check sensor privacy permissions",static_cast<unsigned>(hr)));running_=false;if(SUCCEEDED(init))CoUninitialize();return;}
        ComPtr<IPortableDeviceValues> requested,results;if(SUCCEEDED(CoCreateInstance(CLSID_PortableDeviceValues,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&requested)))){
            requested->SetUnsignedIntegerValue(SENSOR_PROPERTY_CURRENT_REPORT_INTERVAL,10);hr=sensor->SetProperties(requested.Get(),&results);
            Logger::instance().write(SUCCEEDED(hr)?LogLevel::info:LogLevel::warning,std::format(L"Sensor API requested 10 ms interval (0x{:08X})",static_cast<unsigned>(hr)));
        }
        auto rateStart=std::chrono::steady_clock::now();std::uint64_t count{};double rate{};
        while(!stop.stop_requested()&&running_){ComPtr<ISensorDataReport> report;hr=sensor->GetData(&report);if(FAILED(hr)){if(hr==E_ACCESSDENIED)Logger::instance().write(LogLevel::error,L"Sensor API permission denied; check Settings > Privacy & security > Other devices");std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;}
            auto rotation=getNumbers(report.Get(),SENSOR_DATA_TYPE_CUSTOM_VALUE1);auto velocity=getNumbers(report.Get(),SENSOR_DATA_TYPE_CUSTOM_VALUE2);auto reset=getNumbers(report.Get(),SENSOR_DATA_TYPE_CUSTOM_VALUE3);
            if(rotation.size()<3){rotation.clear();for(const auto* key:{&SENSOR_DATA_TYPE_CUSTOM_VALUE1,&SENSOR_DATA_TYPE_CUSTOM_VALUE2,&SENSOR_DATA_TYPE_CUSTOM_VALUE3}){auto v=getNumbers(report.Get(),*key);if(v.empty())break;rotation.push_back(v[0]);}velocity.clear();for(const auto* key:{&SENSOR_DATA_TYPE_CUSTOM_VALUE4,&SENSOR_DATA_TYPE_CUSTOM_VALUE5,&SENSOR_DATA_TYPE_CUSTOM_VALUE6}){auto v=getNumbers(report.Get(),*key);if(v.empty())break;velocity.push_back(v[0]);}reset=getNumbers(report.Get(),SENSOR_DATA_TYPE_CUSTOM_VALUE7);}
            if(rotation.size()>=3&&velocity.size()>=3){TrackingSample sample;sample.rotationVector={rotation[0],rotation[1],rotation[2]};sample.angularVelocity={velocity[0],velocity[1],velocity[2]};sample.hasAngularVelocity=true;if(!reset.empty())sample.resetCounter=static_cast<std::uint8_t>(reset[0]);sample.receivedAt=std::chrono::steady_clock::now();sample.receiveLatencyMs=reportLatencyMs(report.Get());++count;const auto elapsed=std::chrono::duration<double>(sample.receivedAt-rateStart).count();if(elapsed>=1.0){rate=count/elapsed;count=0;rateStart=sample.receivedAt;}sample.packetsPerSecond=rate;if(callback)callback(std::move(sample));}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        sensor.Reset();manager.Reset();if(SUCCEEDED(init))CoUninitialize();running_=false;
    });
    Logger::instance().write(LogLevel::info,L"Windows Sensor API fallback reader started");return true;
}

void SensorBackend::disconnect(){running_=false;if(reader_.joinable()){reader_.request_stop();reader_.join();}}

} // namespace xm5

// =============================================================================
//  Read-only Bluetooth investigation + driver-rebind recovery
// =============================================================================
namespace xm5 {

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

namespace {

struct FindDeviceHandle {
    HBLUETOOTH_DEVICE_FIND value{};
    ~FindDeviceHandle(){if(value)BluetoothFindDeviceClose(value);}
};

struct FindRadioHandle {
    HBLUETOOTH_RADIO_FIND value{};
    ~FindRadioHandle(){if(value)BluetoothFindRadioClose(value);}
};

struct LookupHandle {
    HANDLE value{};
    ~LookupHandle(){if(value)WSALookupServiceEnd(value);}
};

struct WinHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~WinHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};

std::wstring lower(std::wstring_view text){std::wstring result(text);std::ranges::transform(result,result.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});return result;}
bool containsInsensitive(std::wstring_view text,std::wstring_view needle){return needle.empty()||lower(text).find(lower(needle))!=std::wstring::npos;}

std::wstring addressText(BLUETOOTH_ADDRESS address){
    return std::format(L"{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",address.rgBytes[5],address.rgBytes[4],address.rgBytes[3],address.rgBytes[2],address.rgBytes[1],address.rgBytes[0]);
}

// Extracts the 48-bit Bluetooth address embedded in a BTHENUM PnP instance ID.
// Device nodes look like BTHENUM\Dev_F8DF15AABBCC\...; service child nodes end
// with ...&0&F8DF15AABBCC_C00000000. Service GUIDs also contain 12-hex-digit
// runs, so the match is anchored to a "Dev_" prefix or a '&' delimiter.
bool addressFromBthenumId(std::wstring_view id,BLUETOOTH_ADDRESS& address){
    std::wstring text(id);std::ranges::transform(text,text.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});
    const auto isHex=[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'F');};
    const auto tryParse=[&](std::size_t pos){
        if(pos+12>text.size())return false;
        for(std::size_t i=0;i<12;++i)if(!isHex(text[pos+i]))return false;
        if(pos+12<text.size()&&isHex(text[pos+12]))return false;
        std::uint64_t value{};for(std::size_t i=0;i<12;++i){const auto c=text[pos+i];value=(value<<4)|static_cast<std::uint64_t>(c<=L'9'?c-L'0':c-L'A'+10);}
        if(!value)return false;address.ullLong=value;return true;
    };
    if(const auto dev=text.find(L"DEV_");dev!=std::wstring::npos&&tryParse(dev+4))return true;
    for(auto pos=text.find(L'&');pos!=std::wstring::npos;pos=text.find(L'&',pos+1))if(tryParse(pos+1))return true;
    return false;
}

// Enumerates paired Classic Bluetooth devices across all local radios.
std::vector<BLUETOOTH_DEVICE_INFO> pairedClassicDevices(){
    std::vector<BLUETOOTH_DEVICE_INFO> result;
    BLUETOOTH_DEVICE_SEARCH_PARAMS search{};search.dwSize=sizeof(search);search.fReturnAuthenticated=TRUE;search.fReturnRemembered=TRUE;search.fReturnConnected=TRUE;
    BLUETOOTH_DEVICE_INFO device{};device.dwSize=sizeof(device);
    FindDeviceHandle find;find.value=BluetoothFindFirstDevice(&search,&device);
    if(!find.value)return result;
    do{result.push_back(device);device={};device.dwSize=sizeof(device);}while(BluetoothFindNextDevice(find.value,&device));
    return result;
}

bool hasPresentBluetoothHidChild(BLUETOOTH_ADDRESS address){
    auto compactAddress=addressText(address);
    std::erase(compactAddress,L':');
    const auto set=SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES|DIGCF_PRESENT);
    if(set==INVALID_HANDLE_VALUE)return false;
    bool found{};
    for(DWORD index=0;!found;++index){
        SP_DEVINFO_DATA dev{};dev.cbSize=sizeof(dev);
        if(!SetupDiEnumDeviceInfo(set,index,&dev)){if(GetLastError()==ERROR_NO_MORE_ITEMS)break;continue;}
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(set,&dev,instance,MAX_DEVICE_ID_LEN,nullptr))continue;
        const std::wstring_view id(instance);
        if(!id.starts_with(L"HID\\"))continue;
        DEVINST parent{};
        if(CM_Get_Parent(&parent,dev.DevInst,0)!=CR_SUCCESS)continue;
        wchar_t parentId[MAX_DEVICE_ID_LEN]{};
        if(CM_Get_Device_IDW(parent,parentId,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS)continue;
        const std::wstring_view parentText(parentId);
        found=containsInsensitive(parentText,L"BTHENUM\\{00001124-0000-1000-8000-00805F9B34FB}")&&containsInsensitive(parentText,compactAddress);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

std::wstring uuidText(const BTH_LE_UUID& uuid){
    if(uuid.IsShortUuid)return std::format(L"0x{:04X}",uuid.Value.ShortUuid);
    wchar_t buffer[40]{};StringFromGUID2(uuid.Value.LongUuid,buffer,40);return buffer;
}

std::wstring bytesText(const std::uint8_t* data,std::size_t size,std::size_t limit=256){
    std::wostringstream out;out<<std::hex<<std::uppercase<<std::setfill(L'0');
    const auto shown=std::min(size,limit);for(std::size_t i=0;i<shown;++i){if(i)out<<L' ';out<<std::setw(2)<<static_cast<unsigned>(data[i]);}
    if(shown<size)out<<std::format(L" … ({} bytes total)",size);return out.str();
}

std::int64_t hidSigned(std::uint32_t value,unsigned bytes){
    if(bytes==0)return 0;const auto bits=bytes*8u;if(bits>=32)return static_cast<std::int32_t>(value);
    const auto sign=std::uint32_t{1}<<(bits-1);const auto mask=(std::uint32_t{1}<<bits)-1;value&=mask;return static_cast<std::int32_t>((value^sign)-sign);
}

struct HidDescriptorSummary {
    bool sensorApplication{};
    bool hasDescription{};
    bool hasRotation{};
    bool hasVelocity{};
    bool hasReset{};
    std::set<unsigned> reportIds;
};

HidDescriptorSummary describeHidReport(const std::uint8_t* data,std::size_t size,std::wostream& out){
    HidDescriptorSummary summary;std::uint32_t usagePage{},usage{};std::size_t i{};
    out<<L"      HID report descriptor ("<<size<<L" bytes):\n";
    while(i<size){const auto offset=i;const auto prefix=data[i++];if(prefix==0xFE){if(i+2>size)break;const auto length=data[i++];const auto tag=data[i++];out<<std::format(L"        {:04X}: long item tag=0x{:02X} length={}\n",offset,tag,length);i=std::min(size,i+length);continue;}
        const auto sizeCode=prefix&3u;const unsigned length=sizeCode==3?4:sizeCode;const auto type=(prefix>>2)&3u;const auto tag=(prefix>>4)&0xFu;if(i+length>size)break;std::uint32_t value{};for(unsigned b=0;b<length;++b)value|=static_cast<std::uint32_t>(data[i+b])<<(8*b);i+=length;
        if(type==1&&tag==0){usagePage=value;out<<std::format(L"        {:04X}: Usage Page 0x{:X}\n",offset,value);}
        else if(type==2&&tag==0){usage=length==4?value:((usagePage<<16)|value);const auto page=usage>>16,u=usage&0xffff;out<<std::format(L"        {:04X}: Usage 0x{:04X}:0x{:04X}\n",offset,page,u);if(page==0x20&&u==0x0308)summary.hasDescription=true;if(page==0x20&&u==0x0544)summary.hasRotation=true;if(page==0x20&&u==0x0545)summary.hasVelocity=true;if(page==0x20&&u==0x0546)summary.hasReset=true;}
        else if(type==0&&tag==10){const auto page=usage>>16,u=usage&0xffff;out<<std::format(L"        {:04X}: Collection {} for 0x{:04X}:0x{:04X}\n",offset,value,page,u);if(value==1&&page==0x20&&u==0x00E1)summary.sensorApplication=true;usage=0;}
        else if(type==0&&tag==12)out<<std::format(L"        {:04X}: End Collection\n",offset);
        else if(type==1&&tag==8){summary.reportIds.insert(value);out<<std::format(L"        {:04X}: Report ID {}\n",offset,value);}
        else if(type==1&&tag==7)out<<std::format(L"        {:04X}: Report Size {}\n",offset,value);
        else if(type==1&&tag==9)out<<std::format(L"        {:04X}: Report Count {}\n",offset,value);
        else if(type==1&&tag==1)out<<std::format(L"        {:04X}: Logical Minimum {}\n",offset,hidSigned(value,length));
        else if(type==1&&tag==2)out<<std::format(L"        {:04X}: Logical Maximum {}\n",offset,hidSigned(value,length));
        else if(type==1&&tag==3)out<<std::format(L"        {:04X}: Physical Minimum {}\n",offset,hidSigned(value,length));
        else if(type==1&&tag==4)out<<std::format(L"        {:04X}: Physical Maximum {}\n",offset,hidSigned(value,length));
        else if(type==0&&(tag==8||tag==9||tag==11))out<<std::format(L"        {:04X}: {} flags=0x{:X}\n",offset,tag==8?L"Input":tag==9?L"Output":L"Feature",value);
    }
    out<<std::format(L"      Android fields: collection={} description={} rotation={} velocity={} reset={}\n",summary.sensorApplication,summary.hasDescription,summary.hasRotation,summary.hasVelocity,summary.hasReset);
    return summary;
}

void collectSdpStrings(LPBYTE stream,ULONG length,std::vector<std::vector<std::uint8_t>>& strings){
    SDP_ELEMENT_DATA data{};if(BluetoothSdpGetElementData(stream,length,&data)!=ERROR_SUCCESS)return;
    if(data.type==SDP_TYPE_STRING){strings.emplace_back(data.data.string.value,data.data.string.value+data.data.string.length);return;}
    LPBYTE nested=nullptr;ULONG nestedLength{};if(data.type==SDP_TYPE_SEQUENCE){nested=data.data.sequence.value;nestedLength=data.data.sequence.length;}else if(data.type==SDP_TYPE_ALTERNATIVE){nested=data.data.alternative.value;nestedLength=data.data.alternative.length;}else return;
    HBLUETOOTH_CONTAINER_ELEMENT element{};while(true){SDP_ELEMENT_DATA child{};const auto status=BluetoothSdpGetContainerElementData(nested,nestedLength,&element,&child);if(status==ERROR_NO_MORE_ITEMS)break;if(status!=ERROR_SUCCESS)break;if(child.type==SDP_TYPE_STRING)strings.emplace_back(child.data.string.value,child.data.string.value+child.data.string.length);else if(child.type==SDP_TYPE_SEQUENCE)collectSdpStrings(child.data.sequence.value,child.data.sequence.length,strings);else if(child.type==SDP_TYPE_ALTERNATIVE)collectSdpStrings(child.data.alternative.value,child.data.alternative.length,strings);}
}

struct SdpAttributeContext {std::wostream* output{};bool foundDescriptor{};bool androidDescriptor{};};

BOOL CALLBACK sdpAttribute(ULONG id,LPBYTE value,ULONG length,LPVOID rawContext){
    auto& context=*static_cast<SdpAttributeContext*>(rawContext);*context.output<<std::format(L"    SDP attribute 0x{:04X}, {} bytes: {}\n",id,length,bytesText(value,length,id==0x0206?4096:128));
    if(id!=0x0206)return TRUE;std::vector<std::vector<std::uint8_t>> strings;collectSdpStrings(value,length,strings);
    for(const auto& candidate:strings){if(candidate.size()<4)continue;context.foundDescriptor=true;const auto summary=describeHidReport(candidate.data(),candidate.size(),*context.output);context.androidDescriptor|=summary.sensorApplication&&summary.hasDescription&&summary.hasRotation&&summary.hasVelocity&&summary.hasReset;}
    return TRUE;
}

struct ClassicSdpSummary{bool recordReturned{};bool androidDescriptor{};};

ClassicSdpSummary queryClassicSdp(const BLUETOOTH_DEVICE_INFO& device,std::wostream& out){
    ClassicSdpSummary summary;
    SOCKADDR_BTH socketAddress{};socketAddress.addressFamily=AF_BTH;socketAddress.btAddr=device.Address.ullLong;wchar_t contextAddress[64]{};DWORD contextLength=64;
    if(WSAAddressToStringW(reinterpret_cast<LPSOCKADDR>(&socketAddress),sizeof(socketAddress),nullptr,contextAddress,&contextLength)!=0){out<<L"  SDP address formatting failed: "<<WSAGetLastError()<<L"\n";return summary;}
    GUID hidService{0x00001124,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}};WSAQUERYSETW query{};query.dwSize=sizeof(query);query.dwNameSpace=NS_BTH;query.lpServiceClassId=&hidService;query.lpszContext=contextAddress;
    LookupHandle lookup;if(WSALookupServiceBeginW(&query,LUP_FLUSHCACHE,&lookup.value)!=0){out<<L"  Live HID SDP query failed: WSA "<<WSAGetLastError()<<L"\n";return summary;}
    std::vector<std::uint8_t> buffer(64*1024);
    while(true){std::ranges::fill(buffer,std::uint8_t{});auto* result=reinterpret_cast<WSAQUERYSETW*>(buffer.data());result->dwSize=sizeof(*result);DWORD bytes=static_cast<DWORD>(buffer.size());
        const DWORD flags=LUP_RETURN_NAME|LUP_RETURN_COMMENT|LUP_RETURN_ADDR|LUP_RETURN_BLOB;if(WSALookupServiceNextW(lookup.value,flags,&bytes,result)!=0){const auto error=WSAGetLastError();if(error==WSAEFAULT&&bytes>buffer.size()){buffer.resize(bytes);continue;}if(error!=WSA_E_NO_MORE&&error!=WSASERVICE_NOT_FOUND)out<<L"  SDP result error: WSA "<<error<<L"\n";break;}
        summary.recordReturned=true;out<<L"  HID SDP service: "<<(result->lpszServiceInstanceName?result->lpszServiceInstanceName:L"(unnamed)")<<L"\n";
        if(!result->lpBlob||!result->lpBlob->pBlobData){out<<L"    no raw SDP record returned\n";continue;}const auto* record=result->lpBlob->pBlobData;const auto recordSize=result->lpBlob->cbSize;out<<L"    raw record: "<<bytesText(record,recordSize,4096)<<L"\n";SdpAttributeContext attribute{&out};
        if(!BluetoothSdpEnumAttributes(const_cast<LPBYTE>(record),recordSize,sdpAttribute,&attribute))out<<L"    SDP attribute parser failed: "<<GetLastError()<<L"\n";
        out<<std::format(L"    HID descriptor present={} Android Head Tracker descriptor={}\n",attribute.foundDescriptor,attribute.androidDescriptor);
        summary.androidDescriptor|=attribute.androidDescriptor;
    }
    if(!summary.recordReturned)out<<L"  No live HID SDP record was returned.\n";return summary;
}

// Quiet variant used for auto-detection: true when the device's live SDP record
// carries a verified Android Head Tracker HID descriptor. Read-only.
bool deviceAdvertisesAndroidHeadTracker(const BLUETOOTH_DEVICE_INFO& device){
    std::wostringstream sink;return queryClassicSdp(device,sink).androidDescriptor;
}

std::wstring setupString(HDEVINFO set,SP_DEVINFO_DATA& dev,DWORD property){DWORD type{},needed{};SetupDiGetDeviceRegistryPropertyW(set,&dev,property,&type,nullptr,0,&needed);if(!needed)return {};std::vector<std::uint8_t> data(needed);if(!SetupDiGetDeviceRegistryPropertyW(set,&dev,property,&type,data.data(),needed,nullptr))return {};return reinterpret_cast<wchar_t*>(data.data());}

std::vector<BTH_LE_GATT_SERVICE> services(HANDLE device){USHORT count{};auto hr=BluetoothGATTGetServices(device,0,nullptr,&count,BLUETOOTH_GATT_FLAG_NONE);if(hr!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!count)return {};std::vector<BTH_LE_GATT_SERVICE> values(count);if(FAILED(BluetoothGATTGetServices(device,count,values.data(),&count,BLUETOOTH_GATT_FLAG_NONE)))return {};values.resize(count);return values;}
std::vector<BTH_LE_GATT_CHARACTERISTIC> characteristics(HANDLE device,BTH_LE_GATT_SERVICE& service){USHORT count{};auto hr=BluetoothGATTGetCharacteristics(device,&service,0,nullptr,&count,BLUETOOTH_GATT_FLAG_NONE);if(hr!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!count)return {};std::vector<BTH_LE_GATT_CHARACTERISTIC> values(count);if(FAILED(BluetoothGATTGetCharacteristics(device,&service,count,values.data(),&count,BLUETOOTH_GATT_FLAG_NONE)))return {};values.resize(count);return values;}
std::vector<BTH_LE_GATT_DESCRIPTOR> descriptors(HANDLE device,BTH_LE_GATT_CHARACTERISTIC& characteristic){USHORT count{};auto hr=BluetoothGATTGetDescriptors(device,&characteristic,0,nullptr,&count,BLUETOOTH_GATT_FLAG_NONE);if(hr!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!count)return {};std::vector<BTH_LE_GATT_DESCRIPTOR> values(count);if(FAILED(BluetoothGATTGetDescriptors(device,&characteristic,count,values.data(),&count,BLUETOOTH_GATT_FLAG_NONE)))return {};values.resize(count);return values;}

std::vector<std::uint8_t> readCharacteristic(HANDLE device,BTH_LE_GATT_CHARACTERISTIC& characteristic,HRESULT& status){USHORT needed{};status=BluetoothGATTGetCharacteristicValue(device,&characteristic,0,nullptr,&needed,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);if(status!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!needed)return {};std::vector<std::uint8_t> storage(needed);status=BluetoothGATTGetCharacteristicValue(device,&characteristic,needed,reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(storage.data()),nullptr,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);if(FAILED(status))return {};auto* value=reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(storage.data());return {value->Data,value->Data+value->DataSize};}
std::vector<std::uint8_t> readDescriptor(HANDLE device,BTH_LE_GATT_DESCRIPTOR& descriptor,HRESULT& status){USHORT needed{};status=BluetoothGATTGetDescriptorValue(device,&descriptor,0,nullptr,&needed,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);if(status!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!needed)return {};std::vector<std::uint8_t> storage(needed);status=BluetoothGATTGetDescriptorValue(device,&descriptor,needed,reinterpret_cast<PBTH_LE_GATT_DESCRIPTOR_VALUE>(storage.data()),nullptr,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);if(FAILED(status))return {};auto* value=reinterpret_cast<PBTH_LE_GATT_DESCRIPTOR_VALUE>(storage.data());return {value->Data,value->Data+value->DataSize};}

void probeGatt(const BluetoothProbeOptions& options,std::wostream& out){
    const auto set=SetupDiGetClassDevsW(&GUID_BLUETOOTHLE_DEVICE_INTERFACE,nullptr,nullptr,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);if(set==INVALID_HANDLE_VALUE){out<<L"BLE interface enumeration failed: "<<GetLastError()<<L"\n";return;}SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof(iface);unsigned found{};
    for(DWORD index=0;SetupDiEnumDeviceInterfaces(set,nullptr,&GUID_BLUETOOTHLE_DEVICE_INTERFACE,index,&iface);++index){DWORD needed{};SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&needed,nullptr);std::vector<std::uint8_t> detailStorage(needed);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailStorage.data());detail->cbSize=sizeof(*detail);SP_DEVINFO_DATA dev{};dev.cbSize=sizeof(dev);if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,needed,nullptr,&dev))continue;
        wchar_t instance[MAX_DEVICE_ID_LEN]{};SetupDiGetDeviceInstanceIdW(set,&dev,instance,MAX_DEVICE_ID_LEN,nullptr);auto name=setupString(set,dev,SPDRP_FRIENDLYNAME);if(name.empty())name=setupString(set,dev,SPDRP_DEVICEDESC);const bool selected=options.probeAllLeDevices||(!options.nameFilter.empty()&&(containsInsensitive(name,options.nameFilter)||containsInsensitive(instance,options.nameFilter)));out<<std::format(L"BLE device {}: {}\n  instance: {}\n  selected for reads: {}\n",index,name,instance,selected);++found;if(!selected)continue;
        WinHandle handle;handle.value=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);if(handle.value==INVALID_HANDLE_VALUE)handle.value=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);if(handle.value==INVALID_HANDLE_VALUE){out<<L"  open failed: "<<GetLastError()<<L"\n";continue;}
        auto serviceValues=services(handle.value);out<<L"  primary services: "<<serviceValues.size()<<L"\n";for(auto& service:serviceValues){out<<std::format(L"    service {} handle=0x{:04X}\n",uuidText(service.ServiceUuid),service.AttributeHandle);auto chars=characteristics(handle.value,service);for(auto& characteristic:chars){out<<std::format(L"      characteristic {} attr=0x{:04X} value=0x{:04X} properties={}{}{}{}{}\n",uuidText(characteristic.CharacteristicUuid),characteristic.AttributeHandle,characteristic.CharacteristicValueHandle,characteristic.IsReadable?L"R":L"",characteristic.IsWritable?L"W":L"",characteristic.IsWritableWithoutResponse?L"w":L"",characteristic.IsNotifiable?L"N":L"",characteristic.IsIndicatable?L"I":L"");
                if(characteristic.IsReadable){HRESULT hr{};const auto value=readCharacteristic(handle.value,characteristic,hr);out<<L"        read "<<(SUCCEEDED(hr)?L"ok":std::format(L"failed 0x{:08X}",static_cast<unsigned>(hr)))<<L": "<<bytesText(value.data(),value.size(),1024)<<L"\n";}
                auto descs=descriptors(handle.value,characteristic);for(auto& descriptor:descs){out<<std::format(L"        descriptor {} type={} attr=0x{:04X}",uuidText(descriptor.DescriptorUuid),static_cast<unsigned>(descriptor.DescriptorType),descriptor.AttributeHandle);HRESULT hr{};const auto value=readDescriptor(handle.value,descriptor,hr);if(SUCCEEDED(hr))out<<L" value="<<bytesText(value.data(),value.size(),512);else out<<std::format(L" read=0x{:08X}",static_cast<unsigned>(hr));out<<L"\n";}
            }}
    }
    SetupDiDestroyDeviceInfoList(set);if(!found)out<<L"No present BLE device interfaces were exposed by Windows.\n";
}

} // namespace

// Resolves the paired Bluetooth headset name owning a head-tracker HID node by
// walking the PnP parent chain to the BTHENUM node and matching its address
// against the paired-device list.
std::wstring bluetoothNameForHidInstance(std::wstring_view instanceId){
    if(instanceId.empty())return {};
    std::wstring id(instanceId);DEVINST node{};
    if(CM_Locate_DevNodeW(&node,id.data(),CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS)return {};
    BLUETOOTH_ADDRESS address{};bool resolved{};
    for(int depth=0;depth<6&&!resolved;++depth){
        DEVINST parent{};if(CM_Get_Parent(&parent,node,0)!=CR_SUCCESS)break;node=parent;
        wchar_t parentId[MAX_DEVICE_ID_LEN]{};if(CM_Get_Device_IDW(node,parentId,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS)continue;
        const std::wstring_view text(parentId);
        if(!text.starts_with(L"BTHENUM\\"))continue;
        resolved=addressFromBthenumId(text,address);
    }
    if(!resolved)return {};
    for(const auto& device:pairedClassicDevices())if(device.Address.ullLong==address.ullLong)return device.szName;
    return {};
}

std::vector<std::wstring> pairedBluetoothDeviceNames(){
    std::vector<std::wstring> names;
    for(const auto& device:pairedClassicDevices())names.emplace_back(device.szName);
    return names;
}

int runBluetoothProbe(const BluetoothProbeOptions& options,std::wostream& output){
    WSADATA winsock{};if(WSAStartup(MAKEWORD(2,2),&winsock)!=0){output<<L"Winsock initialization failed.\n";return 3;}
    output<<L"Read-only Bluetooth investigation\n=================================\n";BLUETOOTH_DEVICE_SEARCH_PARAMS search{};search.dwSize=sizeof(search);search.fReturnAuthenticated=TRUE;search.fReturnRemembered=TRUE;search.fReturnConnected=TRUE;search.fReturnUnknown=FALSE;search.fIssueInquiry=FALSE;search.cTimeoutMultiplier=2;BLUETOOTH_DEVICE_INFO device{};device.dwSize=sizeof(device);FindDeviceHandle find;find.value=BluetoothFindFirstDevice(&search,&device);unsigned matched{};
    if(find.value){do{if(!containsInsensitive(device.szName,options.nameFilter))continue;++matched;output<<std::format(L"\nClassic device: {} [{}] connected={} authenticated={} remembered={}\n",device.szName,addressText(device.Address),device.fConnected!=FALSE,device.fAuthenticated!=FALSE,device.fRemembered!=FALSE);
        const auto sdp=queryClassicSdp(device,output);
        if(sdp.androidDescriptor)output<<L"  => "<<device.szName<<L" advertises the Android Head Tracker service. This headset should work.\n";
        else if(containsInsensitive(device.szName,L"AirPods"))output<<L"  => AirPods use Apple's proprietary accessory protocol (L2CAP PSM 0x1001), which\n     Windows does not expose to applications. Head tracking cannot be read from\n     AirPods on Windows without a third-party kernel driver. See README > Compatibility.\n";
        device={};device.dwSize=sizeof(device);}while(BluetoothFindNextDevice(find.value,&device));}
    if(!matched)output<<(options.nameFilter.empty()?std::wstring(L"No paired Classic Bluetooth devices were found.\n"):std::format(L"No paired Classic Bluetooth device matched '{}'.\n",options.nameFilter));
    output<<L"\nBLE GATT interfaces\n===================\n";probeGatt(options,output);output<<L"\nNo configuration values were written and no notification subscriptions were enabled.\n";WSACleanup();return matched?0:2;
}

int rebindBluetoothHid(std::wstring_view nameFilter,std::wostream& output){
    BLUETOOTH_FIND_RADIO_PARAMS radioSearch{sizeof(radioSearch)};WinHandle radio;FindRadioHandle findRadio;findRadio.value=BluetoothFindFirstRadio(&radioSearch,&radio.value);
    if(!findRadio.value){output<<L"No Bluetooth radio is available (Win32 "<<GetLastError()<<L").\n";return 3;}
    // With no name filter, auto-detect via read-only SDP which paired device(s)
    // actually advertise the Android Head Tracker descriptor, so the service
    // cycle below never touches an unrelated HID device (mouse, keyboard, ...).
    const bool autoDetect=nameFilter.empty();std::set<unsigned long long> targets;
    if(autoDetect){
        WSADATA winsock{};if(WSAStartup(MAKEWORD(2,2),&winsock)!=0){output<<L"Winsock initialization failed; pass --name to select the headset explicitly.\n";return 3;}
        output<<L"Auto-detecting paired devices that advertise the Android Head Tracker service...\n";
        for(const auto& candidate:pairedClassicDevices())if(deviceAdvertisesAndroidHeadTracker(candidate)){targets.insert(candidate.Address.ullLong);output<<L"  found: "<<candidate.szName<<L" ["<<addressText(candidate.Address)<<L"]\n";}
        WSACleanup();
        if(targets.empty()){output<<L"No paired device advertises the Android Head Tracker service over SDP.\n  - Make sure the headphones are connected and powered on, then retry.\n  - Or pass --name \"<Bluetooth device name>\" to select the headset explicitly.\n  - AirPods and other Apple headphones do not implement this protocol and cannot work.\n";return 2;}
    }
    // Never toggle a live HID service. If Bluetooth's database says the service
    // is enabled but no PnP node exists, disable/enable repairs that stale state.
    GUID hidService{0x00001124,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}};bool matched{};DWORD enableResult=ERROR_NOT_FOUND;
    do{
        BLUETOOTH_DEVICE_SEARCH_PARAMS search{};search.dwSize=sizeof(search);search.fReturnAuthenticated=TRUE;search.fReturnRemembered=TRUE;search.fReturnConnected=TRUE;search.fReturnUnknown=FALSE;search.fIssueInquiry=FALSE;search.hRadio=radio.value;BLUETOOTH_DEVICE_INFO device{};device.dwSize=sizeof(device);FindDeviceHandle find;find.value=BluetoothFindFirstDevice(&search,&device);
        if(find.value){do{const bool wanted=autoDetect?targets.contains(device.Address.ullLong):containsInsensitive(device.szName,nameFilter);if(!wanted){device={};device.dwSize=sizeof(device);continue;}matched=true;const bool liveNode=hasPresentBluetoothHidChild(device.Address);output<<L"Requesting HID service for "<<device.szName<<L" ["<<addressText(device.Address)<<L"]\n  live HID child: "<<(liveNode?L"yes":L"no")<<L"\n";enableResult=BluetoothSetServiceState(radio.value,&device,&hidService,BLUETOOTH_SERVICE_ENABLE);output<<L"  enable result: "<<enableResult<<L"\n";
            if(!liveNode&&(enableResult==ERROR_INVALID_PARAMETER||enableResult==static_cast<DWORD>(E_INVALIDARG))){output<<L"  stale enabled state detected; cycling only the absent HID service\n";const auto disableResult=BluetoothSetServiceState(radio.value,&device,&hidService,BLUETOOTH_SERVICE_DISABLE);output<<L"  disable result: "<<disableResult<<L"\n";if(disableResult==ERROR_SUCCESS||disableResult==ERROR_INVALID_PARAMETER||disableResult==static_cast<DWORD>(E_INVALIDARG)){Sleep(1500);enableResult=BluetoothSetServiceState(radio.value,&device,&hidService,BLUETOOTH_SERVICE_ENABLE);output<<L"  recovery enable result: "<<enableResult<<L"\n";}}
            device={};device.dwSize=sizeof(device);}while(BluetoothFindNextDevice(find.value,&device));}
        if(matched)break;CloseHandle(radio.value);radio.value=INVALID_HANDLE_VALUE;
    }while(BluetoothFindNextRadio(findRadio.value,&radio.value));
    if(!matched){output<<(autoDetect?std::wstring(L"The auto-detected headset was not found during the rebind pass.\n"):std::format(L"No paired Bluetooth device matched '{}'.\n",nameFilter));return 2;}
    if(enableResult==ERROR_SUCCESS){output<<L"HID service was requested; waiting for Plug and Play enumeration.\n";Sleep(5000);return 0;}
    if(enableResult==ERROR_INVALID_PARAMETER||enableResult==static_cast<DWORD>(E_INVALIDARG)){output<<L"HID service is already enabled. No device was removed; power-cycle the headphones.\n";return 0;}
    output<<L"HID service enable failed. No existing device was removed.\n";return 4;
}

int useGenericHidDriver(std::wostream& output){
    const auto set=SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES|DIGCF_PRESENT);if(set==INVALID_HANDLE_VALUE){output<<L"PnP enumeration failed: "<<GetLastError()<<L"\n";return 3;}SP_DEVINFO_DATA selected{};std::wstring selectedInstance,selectedHardwareId;unsigned matches{};
    for(DWORD index=0;;++index){SP_DEVINFO_DATA dev{};dev.cbSize=sizeof(dev);if(!SetupDiEnumDeviceInfo(set,index,&dev)){if(GetLastError()==ERROR_NO_MORE_ITEMS)break;continue;}ULONG status{},problem{};if(CM_Get_DevNode_Status(&status,&problem,dev.DevInst,0)!=CR_SUCCESS||problem!=CM_PROB_FAILED_START)continue;
        DEVINST parent{};if(CM_Get_Parent(&parent,dev.DevInst,0)!=CR_SUCCESS)continue;wchar_t parentId[MAX_DEVICE_ID_LEN]{};if(CM_Get_Device_IDW(parent,parentId,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS||!std::wstring_view(parentId).starts_with(L"BTHENUM\\"))continue;
        DWORD type{},needed{};SetupDiGetDeviceRegistryPropertyW(set,&dev,SPDRP_HARDWAREID,&type,nullptr,0,&needed);if(!needed)continue;std::vector<std::uint8_t> ids(needed);if(!SetupDiGetDeviceRegistryPropertyW(set,&dev,SPDRP_HARDWAREID,&type,ids.data(),needed,nullptr))continue;const auto* current=reinterpret_cast<const wchar_t*>(ids.data());bool headTracker{};std::wstring first;for(;*current;current+=wcslen(current)+1){if(first.empty())first=current;if(containsInsensitive(current,L"UP:0020_U:00E1"))headTracker=true;}if(!headTracker)continue;
        wchar_t instance[MAX_DEVICE_ID_LEN]{};if(!SetupDiGetDeviceInstanceIdW(set,&dev,instance,MAX_DEVICE_ID_LEN,nullptr))continue;++matches;selected=dev;selectedInstance=instance;selectedHardwareId=first;
    }
    SetupDiDestroyDeviceInfoList(set);if(matches!=1){output<<L"Expected exactly one failed Bluetooth Android Head Tracker node; found "<<matches<<L". No driver binding changed.\n";return 2;}
    wchar_t windows[MAX_PATH]{};GetWindowsDirectoryW(windows,MAX_PATH);const auto inputInf=std::wstring(windows)+L"\\INF\\input.inf";BOOL reboot{};output<<L"Binding exact device "<<selectedInstance<<L"\n  hardware ID: "<<selectedHardwareId<<L"\n  inbox INF: "<<inputInf<<L"\n";
    if(!UpdateDriverForPlugAndPlayDevicesW(nullptr,selectedHardwareId.c_str(),inputInf.c_str(),INSTALLFLAG_FORCE|INSTALLFLAG_NONINTERACTIVE,&reboot)){const auto error=GetLastError();output<<L"Generic HID binding failed: "<<error<<L"\n";return error==ERROR_ACCESS_DENIED?5:4;}
    output<<L"Generic HID binding succeeded; reboot required="<<(reboot?L"yes":L"no")<<L"\n";return reboot?1:0;
}

} // namespace xm5

// =============================================================================
//  Orientation filter (smoothing, recenter, gentle drift correction, axis map)
// =============================================================================
namespace xm5 {

class OrientationFilter {
public:
    explicit OrientationFilter(FilterConfig config = {});
    TrackingSample process(TrackingSample sample);
    void recenter();
    void setConfig(FilterConfig config);

private:
    FilterConfig config_;
    Quaternion filtered_{};
    Quaternion center_{};
    Quaternion drift_{};
    Vec3 gyroBias_{};
    Vec3 integratedDrift_{};
    Quaternion latestRaw_{};
    bool initialized_{};
    bool recenterPending_{};
    std::chrono::steady_clock::time_point last_{};
};

OrientationFilter::OrientationFilter(FilterConfig config) : config_(config) {}
void OrientationFilter::setConfig(FilterConfig config) { config_ = config; }
void OrientationFilter::recenter() { recenterPending_ = true; }

TrackingSample OrientationFilter::process(TrackingSample sample) {
    sample.rotationVector = remap(sample.rotationVector, config_.axes);
    sample.angularVelocity = remap(sample.angularVelocity, config_.axes);
    if (sample.hasAcceleration) sample.acceleration = remap(sample.acceleration, config_.axes);
    latestRaw_ = rotationVectorToQuaternion(sample.rotationVector);
    if (!initialized_) { filtered_ = latestRaw_; center_ = latestRaw_; last_ = sample.receivedAt; initialized_ = true; }
    if (recenterPending_) { center_ = latestRaw_; drift_ = {}; gyroBias_={}; integratedDrift_={}; recenterPending_ = false; }
    const auto dt = std::clamp(std::chrono::duration<double>(sample.receivedAt - last_).count(), 0.0, 0.1);
    last_ = sample.receivedAt;
    const auto speed = std::sqrt(sample.angularVelocity.x*sample.angularVelocity.x + sample.angularVelocity.y*sample.angularVelocity.y + sample.angularVelocity.z*sample.angularVelocity.z);
    const auto fast = std::clamp(speed / std::max(0.01, config_.fastMovementRadiansPerSecond), 0.0, 1.0);
    const auto alpha = std::clamp(config_.smoothing + (1.0-config_.smoothing)*fast, 0.001, 1.0);
    filtered_ = slerp(filtered_, latestRaw_, alpha);
    // Estimate only a tiny gyro bias while nearly still. Never pull a deliberately held pose toward center.
    if (speed < 0.08 && config_.driftCorrectionPerSecond > 0.0) {
        const auto adapt=std::clamp(config_.driftCorrectionPerSecond*dt,0.0,0.01);
        gyroBias_.x+=(sample.angularVelocity.x-gyroBias_.x)*adapt;
        gyroBias_.y+=(sample.angularVelocity.y-gyroBias_.y)*adapt;
        gyroBias_.z+=(sample.angularVelocity.z-gyroBias_.z)*adapt;
        integratedDrift_.x+=gyroBias_.x*dt;integratedDrift_.y+=gyroBias_.y*dt;integratedDrift_.z+=gyroBias_.z*dt;
        drift_=rotationVectorToQuaternion(integratedDrift_);
    }
    sample.orientation = multiply(conjugate(drift_), multiply(conjugate(center_), filtered_));
    sample.rotationVector = quaternionToRotationVector(sample.orientation);
    sample.euler = quaternionToEulerDegrees(sample.orientation);
    return sample;
}

} // namespace xm5

// =============================================================================
//  UDP output (OpenTrack doubles + JSON)
// =============================================================================
namespace xm5 {

class UdpOutput {
public:
    UdpOutput();
    ~UdpOutput();
    UdpOutput(const UdpOutput&) = delete;
    UdpOutput& operator=(const UdpOutput&) = delete;
    bool open(std::string host, std::uint16_t port);
    void setDeviceLabel(std::wstring_view name);   // headset name for the JSON "device" field
    void send(const TrackingSample& sample);
    void close();

private:
    SOCKET socket_{INVALID_SOCKET};
    sockaddr_in destination_{};
    std::string deviceJson_{"null"};
};

UdpOutput::UdpOutput() { WSADATA data{}; WSAStartup(MAKEWORD(2,2), &data); }
UdpOutput::~UdpOutput() { close(); WSACleanup(); }

bool UdpOutput::open(std::string host, std::uint16_t port) {
    close();
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) return false;
    destination_.sin_family = AF_INET;
    destination_.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &destination_.sin_addr) != 1) { close(); return false; }
    return true;
}

void UdpOutput::setDeviceLabel(std::wstring_view name) {
    if (name.empty()) { deviceJson_ = "null"; return; }
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes > 0 ? static_cast<std::size_t>(bytes) : 0, '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), utf8.data(), bytes, nullptr, nullptr);
    std::string escaped; escaped.reserve(utf8.size() + 2); escaped.push_back('"');
    for (const char c : utf8) {
        if (c == '"' || c == '\\') escaped.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) escaped.push_back(c);   // control characters are dropped
    }
    escaped.push_back('"'); deviceJson_ = std::move(escaped);
}

void UdpOutput::send(const TrackingSample& s) {
    if (socket_ == INVALID_SOCKET) return;
    // OpenTrack's UDP input expects six native doubles in pose order: x, y, z (translation),
    // then yaw, pitch, roll (rotation, degrees). Translation is always zero -- this protocol
    // reports orientation only -- so the head angles must go in the last three slots.
    const std::array<double, 6> openTrack{0.0, 0.0, 0.0, s.euler.yaw, s.euler.pitch, s.euler.roll};
    sendto(socket_, reinterpret_cast<const char*>(openTrack.data()), static_cast<int>(sizeof(openTrack)), 0,
           reinterpret_cast<const sockaddr*>(&destination_), sizeof(destination_));
    // The JSON datagram follows immediately on port+1 so consumers never need packet sniffing to distinguish formats.
    // gyroscope is radians/second; accelerometer is m/s^2 and is null unless the device actually reports it.
    const auto gyro = s.hasAngularVelocity
        ? std::format("[{:.9g},{:.9g},{:.9g}]", s.angularVelocity.x, s.angularVelocity.y, s.angularVelocity.z)
        : std::string("null");
    const auto accel = s.hasAcceleration
        ? std::format("[{:.9g},{:.9g},{:.9g}]", s.acceleration.x, s.acceleration.y, s.acceleration.z)
        : std::string("null");
    auto json = std::format("{{\"version\":2,\"device\":{},\"rotationVector\":[{:.9g},{:.9g},{:.9g}],\"quaternion\":[{:.9g},{:.9g},{:.9g},{:.9g}],\"yprDegrees\":[{:.9g},{:.9g},{:.9g}],\"gyroscope\":{},\"accelerometer\":{},\"angularVelocity\":{},\"resetCounter\":{},\"packetsPerSecond\":{:.3f},\"receiveLatencyMs\":{:.3f}}}",
        deviceJson_,s.rotationVector.x,s.rotationVector.y,s.rotationVector.z,s.orientation.w,s.orientation.x,s.orientation.y,s.orientation.z,
        s.euler.yaw,s.euler.pitch,s.euler.roll,gyro,accel,gyro,s.resetCounter,s.packetsPerSecond,s.receiveLatencyMs);
    auto jsonDest = destination_; jsonDest.sin_port = htons(static_cast<u_short>(ntohs(destination_.sin_port) + 1));
    sendto(socket_, json.data(), static_cast<int>(json.size()), 0, reinterpret_cast<const sockaddr*>(&jsonDest), sizeof(jsonDest));
}

void UdpOutput::close() { if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; } }

} // namespace xm5

// =============================================================================
//  Diagnostics GUI (Refresh, Repair Tracker, Recenter, live yaw/pitch/roll graph)
// =============================================================================
namespace xm5 {

int runGui(HINSTANCE instance, int showCommand);

namespace {
constexpr UINT kSampleMessage=WM_APP+1;
constexpr UINT kRawMessage=WM_APP+2;
constexpr int kRefresh=1001,kRecenter=1002,kDeviceList=1003,kRepair=1004,kShowAll=1005;

class Window {
public:
    HINSTANCE instance{}; HWND hwnd{}, list{}, details{}, raw{}, stats{}, motion{}, refresh{}, repair{}, recenter{}, invertX{}, invertY{}, invertZ{}, mapping{}, smoothing{}, showAll{};
    HFONT font{}, titleFont{}, sectionFont{}; HICON appIcon{};
    HBRUSH background{}, panel{}, headerBrush{};
    HidBackend hid; SensorBackend sensors; OrientationFilter filter; UdpOutput udp;
    std::vector<DeviceInfo> devices; std::vector<SensorInfo> sensorDevices;
    std::vector<std::size_t> listMap;   // list row -> devices index; devices.size()+i for Sensor API rows; SIZE_MAX for the info row
    std::array<std::vector<float>,3> history; bool connected{}; FilterConfig filterConfig{};
    std::uint16_t udpPort{4242};
    std::wstring headsetName;                                 // resolved Bluetooth name of the connected head tracker
    std::wstring statusText{L"Searching for a head tracker…"};
    COLORREF statusColor{RGB(150,160,176)};

    // Cohesive dark palette shared by the back-buffer paint and the child controls.
    static constexpr COLORREF kWindowBg{RGB(20,23,30)};
    static constexpr COLORREF kHeaderBg{RGB(27,31,42)};
    static constexpr COLORREF kPanelBg{RGB(14,17,23)};
    static constexpr COLORREF kText{RGB(226,231,240)};
    static constexpr COLORREF kMuted{RGB(150,160,176)};
    static constexpr COLORREF kGrid{RGB(46,53,66)};
    static constexpr COLORREF kBorder{RGB(56,64,80)};
    static constexpr COLORREF kAccent{RGB(96,165,255)};
    static constexpr COLORREF kOk{RGB(94,214,140)};
    static constexpr COLORREF kWarn{RGB(255,196,86)};

    ~Window(){hid.disconnect();sensors.disconnect();for(auto f:{font,titleFont,sectionFont})if(f)DeleteObject(f);for(auto b:{background,panel,headerBrush})if(b)DeleteObject(b);if(appIcon)DestroyIcon(appIcon);}

    // Fixed regions, computed from the client size so paint(), layout(), and
    // invalidation always agree exactly.
    RECT clientRect() const { RECT r{}; GetClientRect(hwnd,&r); return r; }
    RECT headerRect() const { const auto r=clientRect(); return RECT{0,0,r.right,64}; }
    RECT outputRect() const { const auto r=clientRect(); return RECT{16,r.bottom-322,r.right-16,r.bottom-230}; }
    RECT graphRect()  const { const auto r=clientRect(); return RECT{16,r.bottom-124,r.right-16,r.bottom-14}; }
    int  listWidth()  const { const auto r=clientRect(); return std::max(300,static_cast<int>(r.right)*17/50); }

    void setStatus(std::wstring text,COLORREF color){
        statusText=std::move(text);statusColor=color;
        const auto h=headerRect();InvalidateRect(hwnd,&h,FALSE);
    }
    void enumerate(){
        hid.disconnect();sensors.disconnect();connected=false;devices=hid.enumerate();sensorDevices=sensors.enumerate();
        // Verified trackers first, unverified custom-sensor candidates second,
        // every other HID collection (mice, touchpads, ...) last.
        std::stable_sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){
            const auto rank=[](const DeviceInfo& d){return d.androidHeadTracker?0:(d.usagePage==kSensorPage&&d.usage==kOtherCustom)?1:2;};
            return rank(a)<rank(b);});
        rebuildList();connectFirst();showDetails(0);
    }
    // Fills the device list. By default only head-tracker candidates are shown;
    // the "Show all devices" checkbox reveals every HID collection for debugging.
    void rebuildList(){
        SendMessageW(list,LB_RESETCONTENT,0,0);listMap.clear();
        const bool everything=SendMessageW(showAll,BM_GETCHECK,0,0)==BST_CHECKED;
        const auto add=[&](const std::wstring& label,std::size_t mapped){SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));listMap.push_back(mapped);};
        for(std::size_t i=0;i<devices.size();++i){
            const auto& d=devices[i];
            const bool candidate=d.usagePage==kSensorPage&&d.usage==kOtherCustom;
            if(!everything&&!candidate)continue;
            const auto& shown=!d.bluetoothName.empty()?d.bluetoothName:(!d.product.empty()?d.product:d.instanceId);
            if(d.androidHeadTracker)add(std::format(L"✔  {}   — Android head tracker",shown),i);
            else if(candidate)add(std::format(L"?  {}   — custom sensor, no Android marker",shown),i);
            else add(std::format(L"HID {:04X}:{:04X}  {}",d.vendorId,d.productId,shown),i);
        }
        for(std::size_t i=0;i<sensorDevices.size();++i){
            const auto& s=sensorDevices[i];
            if(!everything&&!s.androidHeadTracker)continue;
            add(std::format(L"{}  {}   — Windows Sensor API",s.androidHeadTracker?L"✔":L"·",s.friendlyName),devices.size()+i);
        }
        if(listMap.empty())add(L"No head tracker found — press Repair Tracker (admin approval required)",SIZE_MAX);
        SendMessageW(list,LB_SETCURSEL,0,0);
    }
    void showDetails(int selection){
        std::wostringstream o;
        const auto row=static_cast<std::size_t>(selection);
        if(selection<0||row>=listMap.size()||listMap[row]==SIZE_MAX){
            o<<L"No Android Head Tracker HID collection is currently visible.\r\n\r\n"
             <<L"  1.  Make sure the headphones are paired, powered on, and connected.\r\n"
             <<L"  2.  Press Repair Tracker in the toolbar. It asks for one administrator\r\n"
             <<L"      approval and only touches the headset's own HID service — it never\r\n"
             <<L"      installs a custom driver or touches other devices.\r\n"
             <<L"  3.  Tick 'Show all devices' to inspect every HID collection Windows sees.\r\n\r\n"
             <<L"AirPods (and other Apple headphones) cannot work: Apple uses a proprietary\r\n"
             <<L"protocol that Windows does not expose to applications — see the README.";
        }else if(listMap[row]<devices.size()){
            const auto& d=devices[listMap[row]];
            o<<L"Path: "<<d.path<<L"\r\nInstance: "<<d.instanceId<<L"\r\nManufacturer: "<<d.manufacturer<<L"\r\nProduct: "<<d.product<<L"\r\nBluetooth headset: "<<(d.bluetoothName.empty()?L"(unresolved)":d.bluetoothName.c_str());
            o<<std::format(L"\r\nUsage: 0x{:04X}:0x{:04X}   VID/PID: {:04X}:{:04X}\r\nInput bytes: {}   Feature bytes: {}\r\nAndroid description: {}\r\nVerified: {}\r\n\r\nDescriptor fields:\r\n",d.usagePage,d.usage,d.vendorId,d.productId,d.inputReportBytes,d.featureReportBytes,std::wstring(d.sensorDescription.begin(),d.sensorDescription.end()),d.androidHeadTracker?L"yes":L"no");
            for(const auto& f:d.fields)o<<std::format(L"{} id={} usage={:04X}:{:04X} count={} bits={} logical={}..{} physical={}..{} exp={} unit=0x{:X} data={}\r\n",f.feature?L"FEATURE":L"INPUT",f.reportId,f.usagePage,f.usage,f.reportCount,f.bitSize,f.logicalMin,f.logicalMax,f.physicalMin,f.physicalMax,f.unitExponent,f.unit,f.dataIndex);
            for(const auto& v:d.featureValues)o<<L"Feature: "<<std::wstring(v.begin(),v.end())<<L"\r\n";
        }else{
            const auto& s=sensorDevices[listMap[row]-devices.size()];
            o<<L"Windows Sensor API fallback\r\nName: "<<s.friendlyName<<L"\r\nDescription: "<<s.description<<L"\r\nID: "<<s.id<<L"\r\nType: "<<s.type;
        }
        o<<L"\r\n\r\nDiscovery and permission log:\r\n";for(const auto& line:Logger::instance().history())o<<line<<L"\r\n";
        SetWindowTextW(details,o.str().c_str());
    }
    void connectFirst(){
        const auto it=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.androidHeadTracker;});
        if(it!=devices.end()){
            headsetName=!it->bluetoothName.empty()?it->bluetoothName:(!it->product.empty()?it->product:L"head tracker");
            udp.setDeviceLabel(headsetName);
            connected=hid.connect(*it,[w=hwnd](const auto& bytes){PostMessageW(w,kRawMessage,0,reinterpret_cast<LPARAM>(new std::vector<std::uint8_t>(bytes)));},[w=hwnd](TrackingSample s){PostMessageW(w,kSampleMessage,0,reinterpret_cast<LPARAM>(new TrackingSample(std::move(s))));});
            if(connected){
                SetWindowTextW(hwnd,std::format(L"Head Tracker Bridge {} — {}",kVersion,headsetName).c_str());
                setStatus(std::format(L"Tracking {}",headsetName),kOk);
                SetWindowTextW(stats,std::format(L"Connected to {}. Waiting for the first sample…",headsetName).c_str());
            }else setStatus(std::format(L"Found {} but could not open it — see the log in the details pane",headsetName),kWarn);
            return;}
        const auto fallback=std::find_if(sensorDevices.begin(),sensorDevices.end(),[](const auto& s){return s.androidHeadTracker;});
        if(fallback!=sensorDevices.end()){
            headsetName=fallback->friendlyName;udp.setDeviceLabel(headsetName);
            SetWindowTextW(raw,L"Raw packet: unavailable through Windows Sensor API");
            connected=sensors.connect(*fallback,[w=hwnd](TrackingSample s){PostMessageW(w,kSampleMessage,0,reinterpret_cast<LPARAM>(new TrackingSample(std::move(s))));});
            if(connected)setStatus(std::format(L"Tracking {} via the Windows Sensor API",headsetName),kOk);
            return;}
        headsetName.clear();udp.setDeviceLabel({});
        SetWindowTextW(hwnd,std::format(L"Head Tracker Bridge {}",kVersion).c_str());
        bool airpods=false;for(const auto& name:pairedBluetoothDeviceNames())if(containsInsensitive(name,L"AirPods")){airpods=true;break;}
        setStatus(L"No head tracker found — if the headset is paired and on, press Repair Tracker (admin approval required)",kWarn);
        SetWindowTextW(stats,airpods
            ?L"AirPods detected: Apple's protocol is not readable on Windows — only Android-Head-Tracker headsets can work. See README › Compatibility."
            :L"Waiting for an Android Head Tracker HID profile. Power-cycle the headphones if they are already paired.");
    }
    void onSample(std::unique_ptr<TrackingSample> s){
        auto filtered=filter.process(std::move(*s));udp.send(filtered);for(int i=0;i<3;++i){history[i].push_back(static_cast<float>(i==0?filtered.euler.yaw:i==1?filtered.euler.pitch:filtered.euler.roll));if(history[i].size()>360)history[i].erase(history[i].begin());}
        auto text=std::format(L"Yaw {:7.2f}°   Pitch {:7.2f}°   Roll {:7.2f}°     {:6.1f} packets/s     latency: {}",filtered.euler.yaw,filtered.euler.pitch,filtered.euler.roll,filtered.packetsPerSecond,filtered.receiveLatencyMs<0?L"N/A (no device timestamp)":std::format(L"{:.2f} ms",filtered.receiveLatencyMs));SetWindowTextW(stats,text.c_str());
        const auto gyro=filtered.hasAngularVelocity?std::format(L"{:6.2f}, {:6.2f}, {:6.2f} rad/s",filtered.angularVelocity.x,filtered.angularVelocity.y,filtered.angularVelocity.z):std::wstring(L"unavailable");
        const auto accel=filtered.hasAcceleration?std::format(L"{:6.2f}, {:6.2f}, {:6.2f} m/s²",filtered.acceleration.x,filtered.acceleration.y,filtered.acceleration.z):std::wstring(L"not reported by this device");
        SetWindowTextW(motion,std::format(L"Gyroscope  {}        Accelerometer  {}",gyro,accel).c_str());
        const auto rect=graphRect();InvalidateRect(hwnd,&rect,FALSE); // repaint only the live graph -- no whole-window flicker
    }
    void applyControls(){
        static constexpr std::array<std::array<unsigned,3>,6> maps{{{{0,1,2}},{{0,2,1}},{{1,0,2}},{{1,2,0}},{{2,0,1}},{{2,1,0}}}};
        const auto choice=std::clamp<LRESULT>(SendMessageW(mapping,CB_GETCURSEL,0,0),0,5);filterConfig.axes.source=maps[choice];
        filterConfig.axes.sign={SendMessageW(invertX,BM_GETCHECK,0,0)==BST_CHECKED?-1.0:1.0,SendMessageW(invertY,BM_GETCHECK,0,0)==BST_CHECKED?-1.0:1.0,SendMessageW(invertZ,BM_GETCHECK,0,0)==BST_CHECKED?-1.0:1.0};
        filterConfig.smoothing=std::clamp(SendMessageW(smoothing,TBM_GETPOS,0,0)/100.0,0.01,1.0);filter.setConfig(filterConfig);
    }
    void layout(){const auto r=clientRect();const int w=r.right,h=r.bottom;
        // Toolbar row under the header.
        MoveWindow(refresh,16,76,96,30,TRUE);MoveWindow(repair,118,76,158,30,TRUE);MoveWindow(recenter,282,76,96,30,TRUE);
        MoveWindow(mapping,412,80,74,220,TRUE);MoveWindow(invertX,494,80,88,24,TRUE);MoveWindow(invertY,586,80,88,24,TRUE);MoveWindow(invertZ,678,80,88,24,TRUE);MoveWindow(smoothing,772,76,160,30,TRUE);
        // Device list + details columns, above the painted output panel.
        const int lw=listWidth();const int panelTop=136;const int panelBottom=h-334;
        MoveWindow(showAll,16+lw-152,112,152,20,TRUE);
        MoveWindow(list,16,panelTop,lw,panelBottom-panelTop,TRUE);
        MoveWindow(details,16+lw+12,panelTop,w-44-lw,panelBottom-panelTop,TRUE);
        // Telemetry lines between the output panel and the graph.
        MoveWindow(raw,16,h-222,w-32,22,TRUE);MoveWindow(stats,16,h-196,w-32,22,TRUE);MoveWindow(motion,16,h-170,w-32,22,TRUE);}
    void paintSectionLabel(HDC dc,int x,int y,const wchar_t* text){
        SetBkMode(dc,TRANSPARENT);SelectObject(dc,sectionFont);SetTextColor(dc,kMuted);
        TextOutW(dc,x,y,text,static_cast<int>(wcslen(text)));
    }
    // Header band: app icon, title, version, and the live status line.
    void paintHeader(HDC dc){
        const auto hr=headerRect();FillRect(dc,&hr,headerBrush);
        RECT line{hr.left,hr.bottom-1,hr.right,hr.bottom};HBRUSH lb=CreateSolidBrush(kBorder);FillRect(dc,&line,lb);DeleteObject(lb);
        if(appIcon)DrawIconEx(dc,16,16,appIcon,32,32,0,nullptr,DI_NORMAL);
        SetBkMode(dc,TRANSPARENT);
        SelectObject(dc,titleFont);SetTextColor(dc,kText);
        constexpr std::wstring_view title=L"Head Tracker Bridge";
        TextOutW(dc,60,6,title.data(),static_cast<int>(title.size()));
        SIZE ts{};GetTextExtentPoint32W(dc,title.data(),static_cast<int>(title.size()),&ts);
        SelectObject(dc,font);SetTextColor(dc,kMuted);
        const auto version=std::wstring(kVersion);
        TextOutW(dc,60+ts.cx+12,12,version.c_str(),static_cast<int>(version.size()));
        // Status dot + text.
        HBRUSH dot=CreateSolidBrush(statusColor);auto oldBrush=SelectObject(dc,dot);
        HPEN nopen=CreatePen(PS_NULL,0,0);auto oldPen=SelectObject(dc,nopen);
        Ellipse(dc,60,42,70,52);
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(dot);DeleteObject(nopen);
        SetTextColor(dc,kText);
        TextOutW(dc,78,38,statusText.c_str(),static_cast<int>(statusText.size()));
    }
    // Output panel: exactly where the data goes, in plain language.
    void paintOutput(HDC dc){
        const auto r=outputRect();
        FillRect(dc,&r,panel);HBRUSH bb=CreateSolidBrush(kBorder);FrameRect(dc,&r,bb);DeleteObject(bb);
        paintSectionLabel(dc,r.left+12,r.top+8,L"OUTPUT — WHERE THE DATA GOES   (UDP to 127.0.0.1, loopback only: nothing leaves this PC)");
        SetBkMode(dc,TRANSPARENT);SelectObject(dc,font);
        const auto row=[&](int y,const wchar_t* what,const std::wstring& endpoint,const wchar_t* note){
            SetTextColor(dc,kText);TextOutW(dc,r.left+12,y,what,static_cast<int>(wcslen(what)));
            SetTextColor(dc,kAccent);TextOutW(dc,r.left+150,y,endpoint.c_str(),static_cast<int>(endpoint.size()));
            SetTextColor(dc,kMuted);TextOutW(dc,r.left+320,y,note,static_cast<int>(wcslen(note)));
        };
        row(r.top+30,L"OpenTrack",std::format(L"UDP 127.0.0.1:{}",udpPort),L"six doubles (x y z yaw pitch roll) — set OpenTrack's 'UDP over network' input to this port");
        row(r.top+56,L"JSON telemetry",std::format(L"UDP 127.0.0.1:{}",udpPort+1),L"one JSON object per sample, for your own apps — format in docs/PROTOCOL.md");
    }
    // Renders the live graph into an arbitrary DC (used by the back buffer).
    void drawGraph(HDC dc, const RECT& graph){
        FillRect(dc,&graph,panel);
        HBRUSH bb=CreateSolidBrush(kBorder);FrameRect(dc,&graph,bb);DeleteObject(bb);
        const int gw=graph.right-graph.left, gh=graph.bottom-graph.top;
        const int mid=graph.top+gh/2, plot=gh-26; // vertical pixels for ±180°
        SetBkMode(dc,TRANSPARENT);
        // Horizontal grid + degree labels at +180 / +90 / 0 / -90 / -180.
        HPEN grid=CreatePen(PS_SOLID,1,kGrid);auto oldPen=SelectObject(dc,grid);
        SelectObject(dc,font);SetTextColor(dc,kMuted);
        for(int deg=-180;deg<=180;deg+=90){
            const int y=mid-static_cast<int>(deg*(plot/360.0));
            MoveToEx(dc,graph.left+1,y,nullptr);LineTo(dc,graph.right-1,y);
            const auto label=std::format(L"{:>4}°",deg);TextOutW(dc,graph.left+4,y-16,label.c_str(),static_cast<int>(label.size()));
        }
        SelectObject(dc,oldPen);DeleteObject(grid);
        paintSectionLabel(dc,graph.left+64,graph.top+6,L"LIVE ORIENTATION — degrees, Ctrl+Alt+C recenters");
        // Traces.
        const std::array<COLORREF,3> colors{RGB(96,165,255),RGB(94,214,140),RGB(255,120,150)};
        for(int a=0;a<3;++a){if(history[a].size()<2)continue;HPEN pen=CreatePen(PS_SOLID,2,colors[a]);auto old=SelectObject(dc,pen);
            for(std::size_t i=0;i<history[a].size();++i){const auto x=graph.left+static_cast<int>(i*(gw-1)/359.0);const auto y=mid-static_cast<int>(std::clamp(history[a][i],-180.0f,180.0f)*(plot/360.0f));if(i)LineTo(dc,x,y);else MoveToEx(dc,x,y,nullptr);}
            SelectObject(dc,old);DeleteObject(pen);}
        // Legend with live values (top-right swatches).
        SelectObject(dc,font);
        const std::array<const wchar_t*,3> names{L"Yaw",L"Pitch",L"Roll"};int lx=graph.right-330;
        for(int a=0;a<3;++a){
            RECT sw{lx,graph.top+8,lx+12,graph.top+20};HBRUSH b=CreateSolidBrush(colors[a]);FillRect(dc,&sw,b);DeleteObject(b);
            const auto value=history[a].empty()?std::wstring(L"—"):std::format(L"{:+.0f}°",history[a].back());
            const auto text=std::format(L"{} {}",names[a],value);
            SetTextColor(dc,kText);TextOutW(dc,lx+16,graph.top+5,text.c_str(),static_cast<int>(text.size()));lx+=110;
        }
    }
    void paint(){
        PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);
        const auto r=clientRect();
        // Double-buffer: build the frame off-screen, then blit once. This is what
        // eliminates the flicker the live graph used to cause at ~25 fps.
        HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,r.right,r.bottom);auto oldBmp=SelectObject(mem,bmp);
        FillRect(mem,&r,background);
        paintHeader(mem);
        paintSectionLabel(mem,16,114,L"DEVICES");
        paintSectionLabel(mem,16+listWidth()+12,114,L"DETAILS & ACTIVITY LOG");
        paintOutput(mem);
        drawGraph(mem,graphRect());
        BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,oldBmp);DeleteObject(bmp);DeleteDC(mem);
        EndPaint(hwnd,&ps);
    }
};

LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(msg==WM_NCCREATE){self=reinterpret_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_CREATE:{BOOL dark=TRUE;DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
        self->background=CreateSolidBrush(Window::kWindowBg);self->panel=CreateSolidBrush(Window::kPanelBg);self->headerBrush=CreateSolidBrush(Window::kHeaderBg);
        self->font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        self->titleFont=CreateFontW(-26,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        self->sectionFont=CreateFontW(-13,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        self->appIcon=static_cast<HICON>(LoadImageW(self->instance,MAKEINTRESOURCEW(1),IMAGE_ICON,32,32,LR_DEFAULTCOLOR));
        self->refresh=CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefresh)),self->instance,nullptr);self->repair=CreateWindowW(L"BUTTON",L"Repair Tracker",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRepair)),self->instance,nullptr);self->recenter=CreateWindowW(L"BUTTON",L"Recenter",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRecenter)),self->instance,nullptr);self->list=CreateWindowW(L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_NOTIFY|WS_VSCROLL,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeviceList)),self->instance,nullptr);self->details=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->raw=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"Raw packet: waiting",WS_CHILD|WS_VISIBLE|ES_READONLY|ES_AUTOHSCROLL,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->stats=CreateWindowW(L"STATIC",L"Discovering devices…",WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->motion=CreateWindowW(L"STATIC",L"Gyroscope  …        Accelerometer  …",WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd,nullptr,self->instance,nullptr);
        self->mapping=CreateWindowW(WC_COMBOBOXW,L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,0,0,0,0,hwnd,nullptr,self->instance,nullptr);for(const auto* text:{L"XYZ",L"XZY",L"YXZ",L"YZX",L"ZXY",L"ZYX"})SendMessageW(self->mapping,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));SendMessageW(self->mapping,CB_SETCURSEL,2,0);
        self->invertX=CreateWindowW(L"BUTTON",L"Invert X",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->invertY=CreateWindowW(L"BUTTON",L"Invert Y",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->invertZ=CreateWindowW(L"BUTTON",L"Invert Z",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,0,0,hwnd,nullptr,self->instance,nullptr);SendMessageW(self->invertX,BM_SETCHECK,BST_CHECKED,0);SendMessageW(self->invertZ,BM_SETCHECK,BST_CHECKED,0);self->smoothing=CreateWindowW(TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS,0,0,0,0,hwnd,nullptr,self->instance,nullptr);SendMessageW(self->smoothing,TBM_SETRANGE,TRUE,MAKELONG(1,100));SendMessageW(self->smoothing,TBM_SETPOS,TRUE,18);
        self->showAll=CreateWindowW(L"BUTTON",L"Show all devices",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShowAll)),self->instance,nullptr);
        // The UAC shield tells users up front that Repair Tracker will ask for
        // administrator approval (the app itself always runs unelevated).
        SendMessageW(self->repair,BCM_SETSHIELD,0,TRUE);
        // Dark theming: DarkMode_Explorer covers buttons and scrollbars,
        // DarkMode_CFD the combo; checkboxes and the trackbar fall back to
        // classic rendering so the WM_CTLCOLOR palette below applies to them.
        for(auto h:{self->refresh,self->repair,self->recenter,self->list,self->details})SetWindowTheme(h,L"DarkMode_Explorer",nullptr);
        SetWindowTheme(self->mapping,L"DarkMode_CFD",nullptr);
        for(auto h:{self->invertX,self->invertY,self->invertZ,self->showAll,self->smoothing})SetWindowTheme(h,L" ",L" ");
        for(auto h:{self->refresh,self->repair,self->recenter,self->list,self->details,self->raw,self->stats,self->motion,self->mapping,self->invertX,self->invertY,self->invertZ,self->smoothing,self->showAll})SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(self->font),TRUE);
        self->udpPort=4242;self->udp.open("127.0.0.1",self->udpPort);
        RegisterHotKey(hwnd,1,MOD_CONTROL|MOD_ALT,'C');SetTimer(hwnd,1,2000,nullptr);self->enumerate();return 0;}
    case WM_SIZE:self->layout();return 0;case WM_PAINT:self->paint();return 0;
    case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize={1020,760};return 0;}
    case WM_ERASEBKGND:return 1; // the double-buffered WM_PAINT owns the surface; skip the erase that caused flicker
    case WM_CTLCOLORSTATIC:{auto dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,Window::kWindowBg);SetTextColor(dc,Window::kText);return reinterpret_cast<LRESULT>(self->background);}
    case WM_CTLCOLORBTN:{auto dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,Window::kWindowBg);SetTextColor(dc,Window::kText);return reinterpret_cast<LRESULT>(self->background);}
    case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{auto dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,Window::kPanelBg);SetTextColor(dc,Window::kText);return reinterpret_cast<LRESULT>(self->panel);}
    case WM_COMMAND:if(LOWORD(wp)==kRefresh){self->enumerate();return 0;}if(LOWORD(wp)==kRepair){wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd,L"open",executable,L"repair",nullptr,SW_SHOWNORMAL));if(result>32){SetWindowTextW(self->stats,L"Repair started. Approve the Windows prompt; this window will reopen automatically.");PostMessageW(hwnd,WM_CLOSE,0,0);}else MessageBoxW(hwnd,L"Could not start the repair command.",L"Head Tracker Bridge",MB_OK|MB_ICONERROR);return 0;}if(LOWORD(wp)==kRecenter){self->filter.recenter();return 0;}if(LOWORD(wp)==kShowAll){self->rebuildList();self->showDetails(0);return 0;}if(LOWORD(wp)==kDeviceList&&HIWORD(wp)==LBN_SELCHANGE){self->showDetails(static_cast<int>(SendMessageW(self->list,LB_GETCURSEL,0,0)));return 0;}if(reinterpret_cast<HWND>(lp)==self->mapping||reinterpret_cast<HWND>(lp)==self->invertX||reinterpret_cast<HWND>(lp)==self->invertY||reinterpret_cast<HWND>(lp)==self->invertZ){self->applyControls();return 0;}break;
    case WM_HSCROLL:if(reinterpret_cast<HWND>(lp)==self->smoothing){self->applyControls();return 0;}break;
    case WM_HOTKEY:self->filter.recenter();return 0;
    case WM_TIMER:if(!self->hid.connected()&&!self->sensors.connected()&&!self->connected){self->enumerate();}else if(!self->hid.connected()&&!self->sensors.connected()){self->connected=false;self->enumerate();}return 0;
    case kRawMessage:{std::unique_ptr<std::vector<std::uint8_t>> p(reinterpret_cast<std::vector<std::uint8_t>*>(lp));auto text=L"Raw packet: "+hexDump(*p);SetWindowTextW(self->raw,text.c_str());return 0;}
    case kSampleMessage:self->onSample(std::unique_ptr<TrackingSample>(reinterpret_cast<TrackingSample*>(lp)));return 0;
    case WM_DESTROY:UnregisterHotKey(hwnd,1);KillTimer(hwnd,1);PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}

int runGui(HINSTANCE instance,int showCommand){
#ifdef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);Window state;state.instance=instance;WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_HREDRAW|CS_VREDRAW;wc.lpfnWndProc=proc;wc.hInstance=instance;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"XM5HeadTrackerBridgeWindow";
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));if(!wc.hIcon)wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,16,16,LR_DEFAULTCOLOR));
    RegisterClassExW(&wc);const auto title=std::format(L"Head Tracker Bridge {}",kVersion);auto hwnd=CreateWindowExW(0,wc.lpszClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1160,860,nullptr,nullptr,instance,&state);if(!hwnd)return 1;ShowWindow(hwnd,showCommand);UpdateWindow(hwnd);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return static_cast<int>(msg.wParam);}
} // namespace xm5

// =============================================================================
//  CLI entry point + one-click Repair Tracker orchestration
// =============================================================================
namespace {
std::atomic_bool stopRequested{};
BOOL WINAPI consoleHandler(DWORD type){if(type==CTRL_C_EVENT||type==CTRL_CLOSE_EVENT){stopRequested=true;return TRUE;}return FALSE;}
void console(){
    if(!AttachConsole(ATTACH_PARENT_PROCESS)&&GetLastError()==ERROR_INVALID_HANDLE)AllocConsole();
    FILE* ignored{};
    const auto out=GetStdHandle(STD_OUTPUT_HANDLE),err=GetStdHandle(STD_ERROR_HANDLE);
    if(!out||out==INVALID_HANDLE_VALUE||GetFileType(out)==FILE_TYPE_CHAR)freopen_s(&ignored,"CONOUT$","w",stdout);
    if(!err||err==INVALID_HANDLE_VALUE||GetFileType(err)==FILE_TYPE_CHAR)freopen_s(&ignored,"CONOUT$","w",stderr);
    // UTF-8 output mode: device names can contain non-ANSI characters (AirPods
    // default to a curly apostrophe, e.g. "Nicholas's AirPods Pro"), and the
    // default narrow translation poisons wcout at the first one, silently
    // truncating everything after it.
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout),_O_U8TEXT);
    _setmode(_fileno(stderr),_O_U8TEXT);
    SetConsoleCtrlHandler(consoleHandler,TRUE);
}
void printUsage(std::wostream& out){
    out<<L"Head Tracker Bridge "<<xm5::kVersion<<L"\n"
       <<L"Streams head tracking from any headset implementing the Android Head Tracker\n"
       <<L"HID protocol (Sony WH-1000XM5 and others) over UDP (OpenTrack + JSON). The\n"
       <<L"connected headset is auto-detected and named in the output.\n"
       <<L"AirPods are recognised but cannot work: Apple uses a proprietary protocol\n"
       <<L"that Windows does not expose to applications (see README > Compatibility).\n\n"
       <<L"Usage:\n"
       <<L"  xm5-headtracker.exe                       diagnostics GUI (default)\n"
       <<L"  xm5-headtracker.exe bridge [--port 4242] [--seconds N]\n"
       <<L"                             [--axis-map YXZ] [--invert XZ] [--smoothing 0.18]\n"
       <<L"  xm5-headtracker.exe probe [--include-disabled]\n"
       <<L"  xm5-headtracker.exe dump [--seconds N]\n"
       <<L"  xm5-headtracker.exe repair\n"
       <<L"  xm5-headtracker.exe bluetooth-probe [--all-le] [--name FILTER]\n"
       <<L"  xm5-headtracker.exe bluetooth-rebind [--name FILTER]\n"
       <<L"                             (--name defaults to auto-detecting the headset)\n"
       <<L"  xm5-headtracker.exe bluetooth-generic-hid   (run from an elevated prompt)\n"
       <<L"  xm5-headtracker.exe help | version\n\n"
       <<L"bridge sends six little-endian doubles (x, y, z, yaw, pitch, roll) to UDP\n"
       <<L"127.0.0.1:<port> and a JSON datagram to <port>+1. Loopback only; unauthenticated.\n";
}
void printDevice(const xm5::DeviceInfo& d){std::wcout<<std::format(L"HID {}\n  {} {}\n  usage 0x{:04X}:0x{:04X}, VID/PID {:04X}:{:04X}, reports input={} feature={}\n",d.instanceId,d.manufacturer,d.product,d.usagePage,d.usage,d.vendorId,d.productId,d.inputReportBytes,d.featureReportBytes);if(!d.bluetoothName.empty())std::wcout<<L"  Bluetooth headset: "<<d.bluetoothName<<L'\n';std::wcout<<std::format(L"  description: {}\n  verified Android tracker: {}\n",std::wstring(d.sensorDescription.begin(),d.sensorDescription.end()),d.androidHeadTracker?L"yes":L"no");for(const auto& f:d.fields)std::wcout<<std::format(L"    {} id={} {:04X}:{:04X} count={} bits={} logical={}..{} physical={}..{} exp={}\n",f.feature?L"feature":L"input",f.reportId,f.usagePage,f.usage,f.reportCount,f.bitSize,f.logicalMin,f.logicalMax,f.physicalMin,f.physicalMax,f.unitExponent);}

// Names the first paired AirPods (any generation), or empty when none are paired.
// AirPods never implement the Android Head Tracker protocol, so when they are the
// only headphones present the diagnostics say why nothing was found.
std::wstring pairedAirPodsName(){
    for(const auto& name:xm5::pairedBluetoothDeviceNames()){
        std::wstring low(name);std::ranges::transform(low,low.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
        if(low.find(L"airpods")!=std::wstring::npos)return name;
    }
    return {};
}

bool elevated(){
    HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    TOKEN_ELEVATION value{};DWORD bytes{};const bool result=GetTokenInformation(token,TokenElevation,&value,sizeof(value),&bytes)&&value.TokenIsElevated;CloseHandle(token);return result;
}
BOOL CALLBACK closeBridgeWindow(HWND hwnd,LPARAM){wchar_t name[64]{};GetClassNameW(hwnd,name,64);if(std::wstring_view(name)==L"XM5HeadTrackerBridgeWindow")PostMessageW(hwnd,WM_CLOSE,0,0);return TRUE;}
bool trackerAccessible(){xm5::HidBackend hid;const auto devices=hid.enumerate();return std::ranges::any_of(devices,[](const auto& d){return d.androidHeadTracker;});}
int elevatedRepair(){
    std::wcout<<L"Head tracker one-click repair\n=============================\n";
    if(trackerAccessible()){std::wcout<<L"The Android Head Tracker is already accessible. No driver change was needed.\n";return 0;}
    std::wcout<<L"Recreating the headset's Bluetooth HID child (auto-detecting the headset)...\n";
    const auto rebind=xm5::rebindBluetoothHid(L"",std::wcout);if(rebind!=0)return rebind;
    int binding=2;for(int attempt=0;attempt<4&&binding==2;++attempt){if(attempt)std::this_thread::sleep_for(std::chrono::seconds(2));binding=xm5::useGenericHidDriver(std::wcout);}
    if(binding!=0&&binding!=1)return binding;
    for(int attempt=0;attempt<10;++attempt){std::this_thread::sleep_for(std::chrono::seconds(1));if(trackerAccessible()){std::wcout<<L"Repair complete: #AndroidHeadTracker# is accessible.\n";return 0;}}
    std::wcerr<<L"Repair completed its device steps, but the tracker did not become accessible. Power-cycle the headphones and press Repair Tracker once more.\n";return 4;
}
int runRepair(bool launchGui){
    EnumWindows(closeBridgeWindow,0);std::this_thread::sleep_for(std::chrono::milliseconds(500));
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);int result{};
    if(elevated())result=elevatedRepair();
    else{SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS;launch.lpVerb=L"runas";launch.lpFile=executable;launch.lpParameters=L"repair --elevated --no-launch";launch.nShow=SW_HIDE;if(!ShellExecuteExW(&launch)){result=GetLastError()==ERROR_CANCELLED?5:4;}else{WaitForSingleObject(launch.hProcess,INFINITE);DWORD code{};GetExitCodeProcess(launch.hProcess,&code);CloseHandle(launch.hProcess);result=static_cast<int>(code);}}
    if(launchGui)ShellExecuteW(nullptr,L"open",executable,L"gui",nullptr,SW_SHOWNORMAL);return result;
}
}

int wmain(int argc,wchar_t** argv){const std::wstring command=argc>1?argv[1]:L"gui";if(command==L"gui"){FreeConsole();return xm5::runGui(GetModuleHandleW(nullptr),SW_SHOWDEFAULT);}if(command==L"repair"){bool launch=true;for(int i=2;i<argc;++i)if(std::wstring_view(argv[i])==L"--no-launch")launch=false;return runRepair(launch);}console();
    if(command==L"version"||command==L"--version"||command==L"-v"){std::wcout<<L"xm5-headtracker "<<xm5::kVersion<<L'\n';return 0;}
    if(command==L"help"||command==L"--help"||command==L"-h"||command==L"/?"){printUsage(std::wcout);return 0;}
    xm5::Logger::instance().setSink([](xm5::LogLevel,const std::wstring& line){std::wcerr<<line<<L'\n';});
    if(command==L"bluetooth-probe"){xm5::BluetoothProbeOptions options;std::wstring name;for(int i=2;i<argc;++i){const std::wstring_view option=argv[i];if(option==L"--all-le")options.probeAllLeDevices=true;else if(option==L"--name"&&i+1<argc)name=argv[++i];}options.nameFilter=name;return xm5::runBluetoothProbe(options,std::wcout);}
    if(command==L"bluetooth-rebind"){std::wstring name;for(int i=2;i+1<argc;++i)if(std::wstring_view(argv[i])==L"--name")name=argv[++i];return xm5::rebindBluetoothHid(name,std::wcout);}
    if(command==L"bluetooth-generic-hid")return xm5::useGenericHidDriver(std::wcout);
    xm5::HidBackend hid;xm5::SensorBackend sensor;
    bool includeDisabled=false;if(command==L"probe")for(int i=2;i<argc;++i)if(std::wstring_view(argv[i])==L"--include-disabled")includeDisabled=true;
    auto devices=hid.enumerate(!includeDisabled);auto sensors=sensor.enumerate();
    if(command==L"probe"){for(const auto& d:devices)printDevice(d);for(const auto& s:sensors)std::wcout<<L"Sensor API "<<s.friendlyName<<L" | "<<s.description<<L" | "<<s.id<<L'\n';const auto found=std::any_of(devices.begin(),devices.end(),[](const auto& d){return d.androidHeadTracker;})||std::any_of(sensors.begin(),sensors.end(),[](const auto& s){return s.androidHeadTracker;});
        for(const auto& d:devices)if(d.androidHeadTracker)std::wcout<<std::format(L"\nVerified Android head tracker on '{}'.\n",d.bluetoothName.empty()?(d.product.empty()?d.instanceId:d.product):d.bluetoothName);
        if(!found){
            std::wcout<<L"\nNo Android Head Tracker HID sensor was found.\n";
            if(const auto airpods=pairedAirPodsName();!airpods.empty())std::wcout<<L"Note: '"<<airpods<<L"' is paired, but AirPods use Apple's proprietary accessory\nprotocol (L2CAP PSM 0x1001), which Windows does not expose to applications.\nHead tracking cannot be read from AirPods on Windows without a third-party\nkernel driver. See README > Compatibility.\n";
        }
        return found?0:2;}
    auto selected=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.androidHeadTracker;});
    if(command==L"dump"){if(selected==devices.end()){std::wcerr<<L"No verified raw HID head tracker is accessible; Sensor API cannot expose raw packets.\n";return 2;}unsigned seconds{};for(int i=2;i+1<argc;++i)if(std::wstring_view(argv[i])==L"--seconds")seconds=std::wcstoul(argv[++i],nullptr,10);if(!hid.connect(*selected,[](const auto& b){std::wcout<<xm5::hexDump(b)<<L'\n';},[](auto){}))return 3;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);while(!stopRequested&&(!seconds||std::chrono::steady_clock::now()<deadline))std::this_thread::sleep_for(std::chrono::milliseconds(100));hid.disconnect();return 0;}
    if(command==L"bridge"){
        std::uint16_t port=4242;unsigned seconds{};xm5::FilterConfig config;
        for(int i=2;i<argc;++i){
            const std::wstring_view option=argv[i];
            if(option==L"--port"&&i+1<argc){const auto value=std::wcstoul(argv[++i],nullptr,10);if(value<1||value>65534){std::wcerr<<L"--port must be between 1 and 65534 (the JSON stream uses port+1)\n";return 1;}port=static_cast<std::uint16_t>(value);}
            else if(option==L"--seconds"&&i+1<argc)seconds=std::wcstoul(argv[++i],nullptr,10);
            else if(option==L"--smoothing"&&i+1<argc)config.smoothing=std::clamp(std::wcstod(argv[++i],nullptr),0.01,1.0);
            else if(option==L"--invert"&&i+1<argc){const std::wstring axes=argv[++i];config.axes.sign={1.0,1.0,1.0};for(const auto axis:axes){if(axis==L'x'||axis==L'X')config.axes.sign[0]=-1;if(axis==L'y'||axis==L'Y')config.axes.sign[1]=-1;if(axis==L'z'||axis==L'Z')config.axes.sign[2]=-1;}}
            else if(option==L"--axis-map"&&i+1<argc){const std::wstring map=argv[++i];if(map.size()==3){for(unsigned output=0;output<3;++output){const auto c=static_cast<wchar_t>(towlower(map[output]));config.axes.source[output]=c==L'x'?0:c==L'y'?1:2;}}}
        }
        xm5::UdpOutput udp;if(!udp.open("127.0.0.1",port)){std::wcerr<<L"Could not open UDP output\n";return 4;}
        std::wcout<<std::format(L"Streaming head-tracking data:\n  OpenTrack doubles -> UDP 127.0.0.1:{}\n  JSON telemetry    -> UDP 127.0.0.1:{}\n(loopback only; unauthenticated -- do not forward to an untrusted network)\n",port,port+1);
        xm5::OrientationFilter filter(config);auto selectedSensor=std::find_if(sensors.begin(),sensors.end(),[](const auto& s){return s.androidHeadTracker;});
        auto output=[&](xm5::TrackingSample s){auto out=filter.process(std::move(s));udp.send(out);std::wcout<<std::format(L"\rYPR {:7.2f} {:7.2f} {:7.2f}  {:5.1f} pps   ",out.euler.yaw,out.euler.pitch,out.euler.roll,out.packetsPerSecond)<<std::flush;};
        auto connect=[&]{
            if(selected!=devices.end()){
                const auto& name=!selected->bluetoothName.empty()?selected->bluetoothName:selected->product;
                udp.setDeviceLabel(name);
                if(!name.empty())std::wcout<<L"Tracking headset: "<<name<<L'\n';
                return hid.connect(*selected,{},output);
            }
            if(selectedSensor!=sensors.end()){udp.setDeviceLabel(selectedSensor->friendlyName);std::wcout<<L"Tracking headset (Sensor API): "<<selectedSensor->friendlyName<<L'\n';return sensor.connect(*selectedSensor,output);}
            return false;};
        if(!connect()){
            std::wcerr<<L"No Android Head Tracker was found on any connected headset.\n";
            if(const auto airpods=pairedAirPodsName();!airpods.empty())std::wcerr<<L"Note: '"<<airpods<<L"' is paired, but AirPods use Apple's proprietary protocol and\ncannot provide head tracking on Windows. See README > Compatibility.\n";
            return 3;
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);while(!stopRequested&&(!seconds||std::chrono::steady_clock::now()<deadline)){if(!hid.connected()&&!sensor.connected()){std::wcerr<<L"\nDisconnected; probing for reconnection…\n";std::this_thread::sleep_for(std::chrono::seconds(2));devices=hid.enumerate();sensors=sensor.enumerate();selected=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.androidHeadTracker;});selectedSensor=std::find_if(sensors.begin(),sensors.end(),[](const auto& s){return s.androidHeadTracker;});connect();}std::this_thread::sleep_for(std::chrono::milliseconds(100));}
        hid.disconnect();sensor.disconnect();return 0;
    }
    std::wcerr<<L"Unknown command '"<<command<<L"'.\n\n";printUsage(std::wcerr);return 1;
}

// diagnostics_report.cpp
// Windows-side diagnostics: environment/redaction tokens, Windows build string,
// and the CLI `diagnostics` command that enumerates, gathers, redacts, and prints
// a support bundle.
#include "sony_head_tracker/windows_prelude.hpp"

#include "sony_head_tracker/diagnostics.hpp"

#include "sony_head_tracker/app_config.hpp"
#include "sony_head_tracker/bluetooth.hpp"
#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/hid_backend.hpp"
#include "sony_head_tracker/hid_usages.hpp"
#include "sony_head_tracker/logger.hpp"
#include "sony_head_tracker/sensor_api_backend.hpp"
#include "sony_head_tracker/version.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <ostream>
#include <string>
#include <vector>

namespace sony {

namespace {

std::wstring regString(HKEY root, const wchar_t* sub, const wchar_t* name) {
    wchar_t buffer[256]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) return buffer;
    return {};
}

DWORD regDword(HKEY root, const wchar_t* sub, const wchar_t* name) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(root, sub, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) return value;
    return 0;
}

std::wstring axisSummary(const AppConfig& c) {
    const wchar_t axes[] = L"XYZ";
    std::wstring map;
    for (int i = 0; i < 3; ++i) map += axes[c.axes.source[i] % 3];
    std::wstring inverted;
    for (int i = 0; i < 3; ++i) if (c.axes.sign[i] < 0) inverted += axes[i];
    return std::format(L"  Axis map: {}\n  Inverted: {}\n  Smoothing: {:.2f}\n  UDP port: {}\n",
        map, inverted.empty() ? L"(none)" : inverted, c.smoothing, c.udpPort);
}

} // namespace

std::wstring windowsBuildString() {
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const auto display = regString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion");
    const auto build = regString(HKEY_LOCAL_MACHINE, key, L"CurrentBuildNumber");
    const auto ubr = regDword(HKEY_LOCAL_MACHINE, key, L"UBR");
    const int buildNumber = build.empty() ? 0 : _wtoi(build.c_str());
    const wchar_t* name = buildNumber >= 22000 ? L"Windows 11" : L"Windows 10";
    return std::format(L"{} {} (build {}.{})", name, display.empty() ? L"?" : display, build.empty() ? L"?" : build, ubr);
}

RedactionTokens currentRedactionTokens() {
    RedactionTokens t;
    wchar_t user[257]{};
    DWORD userLen = 257;
    if (GetUserNameW(user, &userLen)) t.username = user;
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD computerLen = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(computer, &computerLen)) t.computerName = computer;
    wchar_t profile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) t.userProfile = profile;
    t.deviceNames = pairedBluetoothDeviceNames();
    return t;
}

int runDiagnostics(std::wostream& out) {
    HidBackend hid;
    SensorBackend sensor;
    const auto devices = hid.enumerate();
    const auto sensors = sensor.enumerate();

    const DeviceInfo* tracker = nullptr;
    for (const auto& d : devices) if (d.androidHeadTracker) { tracker = &d; break; }
    const SensorInfo* sensorTracker = nullptr;
    if (!tracker) for (const auto& s : sensors) if (s.androidHeadTracker) { sensorTracker = &s; break; }

    DiagnosticsInput in;
    in.appVersion = std::wstring(kVersion);
    in.windowsBuild = windowsBuildString();
    in.destinationPort = L"4242 / 4243";
    if (tracker) {
        in.backend = L"Raw HID";
        in.headsetModel = !tracker->product.empty() ? tracker->product : tracker->instanceId;
        in.hidUsage = std::format(L"0x{:04X}:0x{:04X}", tracker->usagePage, tracker->usage);
        in.descriptor = std::wstring(tracker->sensorDescription.begin(), tracker->sensorDescription.end());
        for (const auto& f : tracker->fields)
            if (f.usagePage == kSensorPage && (f.usage == kAngularVelocity || f.usage == kAngularVelocityVector || f.usage == kAngularVelocityX))
                in.angularVelocity = true;
    } else if (sensorTracker) {
        in.backend = L"Windows Sensor API";
        in.headsetModel = sensorTracker->friendlyName;
        in.descriptor = sensorTracker->description;
    } else {
        in.backend = L"none";
    }
    in.settings = axisSummary(loadAppConfig());

    auto history = Logger::instance().history();
    for (const auto& l : history) if (l.find(L" ERROR ") != std::wstring::npos) in.lastError = l;
    const std::size_t keep = std::min<std::size_t>(history.size(), 40);
    in.logLines.assign(history.end() - static_cast<std::ptrdiff_t>(keep), history.end());

    out << redactDiagnostics(formatDiagnostics(in), currentRedactionTokens());
    return 0;
}

} // namespace sony

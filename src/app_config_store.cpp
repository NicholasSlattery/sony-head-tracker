// app_config_store.cpp
// Windows persistence for AppConfig: resolves %LOCALAPPDATA%\SonyHeadTracker,
// and reads/writes/imports/exports the JSON produced by app_config.cpp.
#include "sony_head_tracker/windows_prelude.hpp"

#include "sony_head_tracker/app_config.hpp"

#include <ShlObj.h>

#include <fstream>
#include <sstream>
#include <string>

namespace sony {

namespace {

std::wstring localAppData() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) result = path;
    if (path) CoTaskMemFree(path);
    return result;
}

bool writeFile(const std::wstring& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

std::string readFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

std::wstring appConfigPath() {
    const auto base = localAppData();
    if (base.empty()) return {};
    return base + L"\\SonyHeadTracker\\config.json";
}

AppConfig loadAppConfig() {
    const auto path = appConfigPath();
    if (path.empty()) return {};
    const auto text = readFile(path);
    if (text.empty()) return {};
    return appConfigFromJson(text);
}

bool saveAppConfig(const AppConfig& config) {
    const auto base = localAppData();
    if (base.empty()) return false;
    const auto dir = base + L"\\SonyHeadTracker";
    CreateDirectoryW(dir.c_str(), nullptr);   // succeeds or already exists
    return writeFile(dir + L"\\config.json", appConfigToJson(config));
}

bool exportAppConfig(const AppConfig& config, const std::wstring& path) {
    return writeFile(path, appConfigToJson(config));
}

bool importAppConfig(AppConfig& config, const std::wstring& path) {
    const auto text = readFile(path);
    if (text.empty()) return false;
    config = appConfigFromJson(text);
    return true;
}

} // namespace sony

// app_config_store_macos.cpp
// macOS persistence for AppConfig under the user's Application Support folder.
#include "sony_head_tracker/app_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <string>
#include <unistd.h>

namespace sony {

namespace {

std::filesystem::path homeDirectory() {
    if (const char* home = std::getenv("HOME"); home && *home) return home;
    if (const auto* user = getpwuid(getuid()); user && user->pw_dir) return user->pw_dir;
    return {};
}

std::filesystem::path configPath() {
    const auto home = homeDirectory();
    if (home.empty()) return {};
    return home / "Library" / "Application Support" / "SonyHeadTracker" / "config.json";
}

bool writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

} // namespace

std::wstring appConfigPath() {
    const auto path = configPath();
    return path.empty() ? std::wstring{} : path.wstring();
}

AppConfig loadAppConfig() {
    const auto path = configPath();
    if (path.empty()) return {};
    const auto text = readFile(path);
    return text.empty() ? AppConfig{} : appConfigFromJson(text);
}

bool saveAppConfig(const AppConfig& config) {
    const auto path = configPath();
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    return writeFile(path, appConfigToJson(config));
}

bool exportAppConfig(const AppConfig& config, const std::wstring& path) {
    return writeFile(std::filesystem::path(path), appConfigToJson(config));
}

bool importAppConfig(AppConfig& config, const std::wstring& path) {
    const auto text = readFile(std::filesystem::path(path));
    if (text.empty()) return false;
    config = appConfigFromJson(text);
    return true;
}

} // namespace sony

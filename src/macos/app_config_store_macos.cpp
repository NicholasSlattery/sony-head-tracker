// app_config_store_macos.cpp
// macOS persistence for AppConfig under the user's Application Support folder.
#include "sony_head_tracker/app_config.hpp"
#include "sony_head_tracker/macos_atomic_file.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <pwd.h>
#include <string>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>

namespace sony {

namespace {

constexpr mode_t kPrivateDirectoryMode = 0700;
constexpr mode_t kPrivateFileMode = 0600;
constexpr std::size_t kMaximumConfigBytes = 1024 * 1024;

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

bool writeExportFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}

std::string readImportFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

namespace macos {
namespace {

bool ensurePrivateDirectory(const std::filesystem::path& directory) {
    if (directory.empty()) return false;
    std::filesystem::path current = directory.root_path();
    for (const auto& component : directory.relative_path()) {
        if (component == "." || component == "..") return false;
        current /= component;
        if (::mkdir(current.c_str(), kPrivateDirectoryMode) != 0 && errno != EEXIST) return false;
        struct stat status{};
        if (::lstat(current.c_str(), &status) != 0 || S_ISLNK(status.st_mode) ||
            !S_ISDIR(status.st_mode)) {
            return false;
        }
    }
    return ::chmod(directory.c_str(), kPrivateDirectoryMode) == 0;
}

bool finalPathIsSafe(int directoryFd, const std::string& filename) {
    struct stat status{};
    if (::fstatat(directoryFd, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
        return S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode);
    }
    return errno == ENOENT;
}

std::string randomSuffix() {
    std::array<std::uint8_t, 16> bytes{};
    arc4random_buf(bytes.data(), bytes.size());
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

bool writeAll(int file, std::string_view content, const AtomicFileHooks& hooks) {
    std::size_t offset{};
    while (offset < content.size()) {
        const auto remaining = content.size() - offset;
        const auto written = hooks.write
            ? hooks.write(file, content.data() + offset, remaining)
            : static_cast<std::ptrdiff_t>(::write(file, content.data() + offset, remaining));
        if (written > 0 && static_cast<std::size_t>(written) <= remaining) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

} // namespace

bool writePrivateFileAtomically(const std::filesystem::path& path,
                                std::string_view content,
                                const AtomicFileHooks& hooks) {
    const auto directory = path.parent_path();
    const auto filename = path.filename().string();
    if (filename.empty() || filename == "." || filename == ".." ||
        !ensurePrivateDirectory(directory)) {
        return false;
    }

    const int directoryFd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) return false;
    const auto closeDirectory = [&] { ::close(directoryFd); };
    if (!finalPathIsSafe(directoryFd, filename)) {
        closeDirectory();
        return false;
    }

    int temporaryFd = -1;
    std::string temporaryName;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const auto suffix = hooks.temporarySuffix ? hooks.temporarySuffix() : randomSuffix();
        temporaryName = "." + filename + ".tmp." + suffix;
        temporaryFd = ::openat(directoryFd, temporaryName.c_str(),
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                               kPrivateFileMode);
        if (temporaryFd >= 0) break;
        if (errno != EEXIST) {
            closeDirectory();
            return false;
        }
    }
    if (temporaryFd < 0) {
        closeDirectory();
        return false;
    }

    const auto cleanup = [&] {
        if (temporaryFd >= 0) ::close(temporaryFd);
        ::unlinkat(directoryFd, temporaryName.c_str(), 0);
        closeDirectory();
    };
    if (!writeAll(temporaryFd, content, hooks) ||
        (hooks.fsync ? hooks.fsync(temporaryFd) : ::fsync(temporaryFd)) != 0 ||
        ::fchmod(temporaryFd, kPrivateFileMode) != 0) {
        cleanup();
        return false;
    }
    if (::close(temporaryFd) != 0) {
        temporaryFd = -1;
        cleanup();
        return false;
    }
    temporaryFd = -1;

    const auto renamed = hooks.renameAt
        ? hooks.renameAt(directoryFd, temporaryName.c_str(), directoryFd, filename.c_str(), 0)
        : ::renameat(directoryFd, temporaryName.c_str(), directoryFd, filename.c_str());
    if (renamed != 0) {
        cleanup();
        return false;
    }
    if ((hooks.fsync ? hooks.fsync(directoryFd) : ::fsync(directoryFd)) != 0) {
        closeDirectory();
        return false;
    }
    closeDirectory();
    return true;
}

std::string readPrivateFile(const std::filesystem::path& path) {
    const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0) return {};
    struct stat status{};
    if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > kMaximumConfigBytes) {
        ::close(file);
        return {};
    }
    std::string content(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset{};
    while (offset < content.size()) {
        const auto count = ::read(file, content.data() + offset, content.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        ::close(file);
        return {};
    }
    ::close(file);
    return content;
}

} // namespace macos

std::wstring appConfigPath() {
    const auto path = configPath();
    return path.empty() ? std::wstring{} : path.wstring();
}

AppConfig loadAppConfig() {
    const auto path = configPath();
    if (path.empty()) return {};
    const auto text = macos::readPrivateFile(path);
    return text.empty() ? AppConfig{} : appConfigFromJson(text);
}

bool saveAppConfig(const AppConfig& config) {
    const auto path = configPath();
    return !path.empty() && macos::writePrivateFileAtomically(path, appConfigToJson(config));
}

bool exportAppConfig(const AppConfig& config, const std::wstring& path) {
    return writeExportFile(std::filesystem::path(path), appConfigToJson(config));
}

bool importAppConfig(AppConfig& config, const std::wstring& path) {
    const auto text = readImportFile(std::filesystem::path(path));
    if (text.empty()) return false;
    config = appConfigFromJson(text);
    return true;
}

} // namespace sony

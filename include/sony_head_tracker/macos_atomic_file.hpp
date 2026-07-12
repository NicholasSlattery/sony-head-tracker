// macos_atomic_file.hpp
// Private, same-directory atomic file replacement used for macOS configuration.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace sony::macos {

struct AtomicFileHooks {
    // Test seams only. Empty hooks select the POSIX system calls.
    std::function<std::ptrdiff_t(int, const void*, std::size_t)> write;
    std::function<int(int)> fsync;
    std::function<int(int, const char*, int, const char*, int)> renameAt;
    std::function<std::string()> temporarySuffix;
};

// Creates or replaces a regular final file without following a symlink. The
// completed file is mode 0600 and the containing directory is mode 0700.
bool writePrivateFileAtomically(const std::filesystem::path& path,
                                std::string_view content,
                                const AtomicFileHooks& hooks = {});

// Reads only a regular, non-symlink final file. An unreadable or unsafe file
// yields an empty string, matching the configuration layer's default behavior.
std::string readPrivateFile(const std::filesystem::path& path);

} // namespace sony::macos

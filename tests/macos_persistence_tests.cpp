#include "test_framework.hpp"

#include "sony_head_tracker/macos_atomic_file.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("sony-head-tracker-atomic-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};

bool hasTemporaryResidue(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().find(".tmp.") != std::string::npos) return true;
    }
    return false;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(macos_atomic_config_round_trip_is_private_and_has_no_residue) {
    TemporaryDirectory temporary;
    const auto config = temporary.path / "config.json";
    CHECK(sony::macos::writePrivateFileAtomically(config, "new"));
    CHECK(sony::macos::readPrivateFile(config) == "new");
    struct stat directoryStatus{};
    struct stat fileStatus{};
    CHECK(stat(temporary.path.c_str(), &directoryStatus) == 0);
    CHECK(stat(config.c_str(), &fileStatus) == 0);
    CHECK((directoryStatus.st_mode & 0777) == 0700);
    CHECK((fileStatus.st_mode & 0777) == 0600);
    CHECK(!hasTemporaryResidue(temporary.path));
}

TEST(macos_atomic_config_rejects_final_symlinks_and_invalid_parents) {
    TemporaryDirectory temporary;
    const auto target = temporary.path / "target";
    const auto config = temporary.path / "config.json";
    {
        std::ofstream output(target);
        output << "old";
    }
    CHECK(symlink(target.c_str(), config.c_str()) == 0);
    CHECK(!sony::macos::writePrivateFileAtomically(config, "new"));
    CHECK(readText(target) == "old");

    const auto parentFile = temporary.path / "not-a-directory";
    {
        std::ofstream output(parentFile);
        output << "x";
    }
    CHECK(!sony::macos::writePrivateFileAtomically(parentFile / "config.json", "new"));
}

TEST(macos_atomic_config_handles_temp_collisions_and_partial_writes) {
    TemporaryDirectory temporary;
    const auto config = temporary.path / "config.json";
    const auto collision = temporary.path / ".config.json.tmp.collision";
    {
        std::ofstream output(collision);
        output << "reserved";
    }
    unsigned suffixCall{};
    sony::macos::AtomicFileHooks hooks;
    hooks.temporarySuffix = [&] { return suffixCall++ == 0 ? "collision" : "fresh"; };
    hooks.write = [](int file, const void* bytes, std::size_t count) {
        const auto chunk = std::min<std::size_t>(count, 1);
        return static_cast<std::ptrdiff_t>(::write(file, bytes, chunk));
    };
    CHECK(sony::macos::writePrivateFileAtomically(config, "new", hooks));
    CHECK(readText(config) == "new");
    CHECK(readText(collision) == "reserved");
    CHECK(!std::filesystem::exists(temporary.path / ".config.json.tmp.fresh"));
}

TEST(macos_atomic_config_failures_preserve_existing_content_and_clean_up) {
    TemporaryDirectory temporary;
    const auto config = temporary.path / "config.json";
    CHECK(sony::macos::writePrivateFileAtomically(config, "old"));

    sony::macos::AtomicFileHooks writeFailure;
    writeFailure.write = [](int, const void*, std::size_t) {
        errno = EIO;
        return std::ptrdiff_t{-1};
    };
    CHECK(!sony::macos::writePrivateFileAtomically(config, "new", writeFailure));
    CHECK(readText(config) == "old");
    CHECK(!hasTemporaryResidue(temporary.path));

    sony::macos::AtomicFileHooks renameFailure;
    renameFailure.renameAt = [](int, const char*, int, const char*, int) {
        errno = EIO;
        return -1;
    };
    CHECK(!sony::macos::writePrivateFileAtomically(config, "new", renameFailure));
    CHECK(readText(config) == "old");
    CHECK(!hasTemporaryResidue(temporary.path));
}

// logger.hpp
// Thread-safe process-wide logger with a pluggable sink and bounded history.
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace sony {

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

} // namespace sony

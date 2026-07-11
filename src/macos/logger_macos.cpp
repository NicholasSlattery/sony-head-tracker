// logger_macos.cpp
// macOS logger implementation. Keeps the same bounded history and sink contract
// as the Windows logger while writing a readable stderr line for CLI builds.
#include "sony_head_tracker/logger.hpp"

#include <chrono>
#include <format>
#include <iostream>

namespace sony {

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
    const wchar_t* label = level == LogLevel::error ? L"ERROR"
        : level == LogLevel::warning ? L"WARN"
        : level == LogLevel::debug ? L"DEBUG" : L"INFO";
    auto line = std::format(L"[{:%H:%M:%S}] {:5} {}", now, label, message);
    Sink sink;
    {
        std::scoped_lock lock(mutex_);
        history_.push_back(line);
        if (history_.size() > 2000) history_.erase(history_.begin(), history_.begin() + 500);
        sink = sink_;
    }
    std::wcerr << line << L'\n';
    if (sink) sink(level, line);
}

std::vector<std::wstring> Logger::history() const {
    std::scoped_lock lock(mutex_);
    return history_;
}

// Kept for the shared logger interface. macOS callers should prefer their
// native error domain/message when they have one; this fallback is only used by
// shared code that still carries an unsigned numeric error value.
std::wstring windowsError(unsigned long code) {
    return std::format(L"macOS error {}", code);
}

} // namespace sony

// logger.cpp
#include "sony_head_tracker/windows_prelude.hpp"

#include "sony_head_tracker/logger.hpp"

#include <chrono>
#include <format>

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

} // namespace sony

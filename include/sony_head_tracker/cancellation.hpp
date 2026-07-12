// cancellation.hpp
// Small C++20-compatible cooperative cancellation primitive.  Unlike
// std::stop_token, this is available with the libc++ shipped by Xcode 16.
#pragma once

#include <atomic>

namespace sony {

class CancellationFlag {
public:
    void requestStop() noexcept {
        requested_.store(true, std::memory_order_release);
    }

    void reset() noexcept {
        requested_.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return requested_.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool requested_{};
};

} // namespace sony

// hid_backend.hpp
// Raw HID backend: enumerates HID top-level collections, verifies the Android
// Head Tracker marker, enables reporting, and streams normalized MotionSamples.
// The public interface exposes no Windows types (the OS state lives in the
// forward-declared Context), so this header is includable anywhere.
#pragma once

#include "sony_head_tracker/cancellation.hpp"
#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sony {

class HidBackend {
public:
    using RawCallback = std::function<void(const std::vector<std::uint8_t>&)>;
    using SampleCallback = std::function<void(MotionSample)>;
    // Opaque platform state. Public only so platform translation-unit helpers
    // can name the incomplete type; callers cannot inspect its definition.
    struct Context;

    HidBackend();
    ~HidBackend();
    std::vector<DeviceInfo> enumerate(bool presentInterfacesOnly = true);
    bool connect(const DeviceInfo& device, RawCallback raw, SampleCallback sample);
    void disconnect();
    [[nodiscard]] bool connected() const { return running_; }

private:
    std::unique_ptr<Context> context_;
    std::thread reader_;
    CancellationFlag readerStop_;
    std::atomic_bool running_{};
};

std::wstring hexDump(const std::vector<std::uint8_t>& bytes);

} // namespace sony

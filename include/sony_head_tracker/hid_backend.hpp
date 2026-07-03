// hid_backend.hpp
// Raw HID backend: enumerates HID top-level collections, verifies the Android
// Head Tracker marker, enables reporting, and streams normalized MotionSamples.
// The public interface exposes no Windows types (the OS state lives in the
// forward-declared Context), so this header is includable anywhere.
#pragma once

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

    HidBackend();
    ~HidBackend();
    std::vector<DeviceInfo> enumerate(bool presentInterfacesOnly = true);
    bool connect(const DeviceInfo& device, RawCallback raw, SampleCallback sample);
    void disconnect();
    [[nodiscard]] bool connected() const { return running_; }

private:
    struct Context;
    std::unique_ptr<Context> context_;
    std::jthread reader_;
    std::atomic_bool running_{};
};

std::wstring hexDump(const std::vector<std::uint8_t>& bytes);

} // namespace sony

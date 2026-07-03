// sensor_api_backend.hpp
// Windows Sensor API fallback backend. Streams normalized MotionSamples from a
// custom sensor when the raw HID path is unavailable. No Windows types leak
// through the public interface.
#pragma once

#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/types.hpp"

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace sony {

class SensorBackend {
public:
    using SampleCallback = std::function<void(MotionSample)>;
    ~SensorBackend();
    std::vector<SensorInfo> enumerate();
    bool connect(const SensorInfo& sensor, SampleCallback sample);
    void disconnect();
    [[nodiscard]] bool connected() const { return running_; }

private:
    std::jthread reader_;
    std::atomic_bool running_{};
};

} // namespace sony

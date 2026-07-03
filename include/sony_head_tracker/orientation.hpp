// orientation.hpp
// Orientation filter: adaptive smoothing, recenter, gentle drift correction and
// axis mapping. Operates purely on MotionSample; no Windows, no hardware.
#pragma once

#include "sony_head_tracker/types.hpp"

#include <chrono>

namespace sony {

class OrientationFilter {
public:
    explicit OrientationFilter(FilterConfig config = {});
    MotionSample process(MotionSample sample);
    void recenter();
    void setConfig(FilterConfig config);

private:
    FilterConfig config_;
    Quaternion filtered_{};
    Quaternion center_{};
    Quaternion drift_{};
    Vec3 gyroBias_{};
    Vec3 integratedDrift_{};
    Quaternion latestRaw_{};
    bool initialized_{};
    bool recenterPending_{};
    std::chrono::steady_clock::time_point last_{};
};

} // namespace sony

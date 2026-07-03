// orientation.cpp
// Orientation filter (smoothing, recenter, gentle drift correction, axis map).
// Pure: operates only on MotionSample and the maths helpers.
#include "sony_head_tracker/orientation.hpp"

#include "sony_head_tracker/math.hpp"

#include <algorithm>
#include <cmath>

namespace sony {

OrientationFilter::OrientationFilter(FilterConfig config) : config_(config) {}
void OrientationFilter::setConfig(FilterConfig config) { config_ = config; }
void OrientationFilter::recenter() { recenterPending_ = true; }

MotionSample OrientationFilter::process(MotionSample sample) {
    sample.rotationVector = remap(sample.rotationVector, config_.axes);
    if (sample.angularVelocity) sample.angularVelocity = remap(*sample.angularVelocity, config_.axes);
    if (sample.acceleration) sample.acceleration = remap(*sample.acceleration, config_.axes);
    // Missing angular velocity behaves as zero for the still-detection heuristic,
    // so an orientation-only device simply forgoes drift correction.
    const Vec3 angular = sample.angularVelocity.value_or(Vec3{});
    latestRaw_ = rotationVectorToQuaternion(sample.rotationVector);
    if (!initialized_) { filtered_ = latestRaw_; center_ = latestRaw_; last_ = sample.receivedAt; initialized_ = true; }
    if (recenterPending_) { center_ = latestRaw_; drift_ = {}; gyroBias_={}; integratedDrift_={}; recenterPending_ = false; }
    const auto dt = std::clamp(std::chrono::duration<double>(sample.receivedAt - last_).count(), 0.0, 0.1);
    last_ = sample.receivedAt;
    const auto speed = std::sqrt(angular.x*angular.x + angular.y*angular.y + angular.z*angular.z);
    const auto fast = std::clamp(speed / std::max(0.01, config_.fastMovementRadiansPerSecond), 0.0, 1.0);
    const auto alpha = std::clamp(config_.smoothing + (1.0-config_.smoothing)*fast, 0.001, 1.0);
    filtered_ = slerp(filtered_, latestRaw_, alpha);
    // Estimate only a tiny gyro bias while nearly still. Never pull a deliberately held pose toward center.
    if (speed < 0.08 && config_.driftCorrectionPerSecond > 0.0) {
        const auto adapt=std::clamp(config_.driftCorrectionPerSecond*dt,0.0,0.01);
        gyroBias_.x+=(angular.x-gyroBias_.x)*adapt;
        gyroBias_.y+=(angular.y-gyroBias_.y)*adapt;
        gyroBias_.z+=(angular.z-gyroBias_.z)*adapt;
        integratedDrift_.x+=gyroBias_.x*dt;integratedDrift_.y+=gyroBias_.y*dt;integratedDrift_.z+=gyroBias_.z*dt;
        drift_=rotationVectorToQuaternion(integratedDrift_);
    }
    sample.orientation = multiply(conjugate(drift_), multiply(conjugate(center_), filtered_));
    sample.rotationVector = quaternionToRotationVector(sample.orientation);
    sample.euler = quaternionToEulerDegrees(sample.orientation);
    return sample;
}

} // namespace sony

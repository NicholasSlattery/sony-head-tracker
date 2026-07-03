// types.hpp
// Core value types shared by every layer. Deliberately free of any Windows or
// Bluetooth dependency so the tracking maths and serialisation can be unit
// tested without hardware.
#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <optional>

namespace sony {

struct Vec3 {
    double x{};
    double y{};
    double z{};
    // Bounds-checked: an out-of-range index is a programming error, not a request for z.
    constexpr double& operator[](std::size_t i) { assert(i < 3); return i == 0 ? x : (i == 1 ? y : z); }
    constexpr double operator[](std::size_t i) const { assert(i < 3); return i == 0 ? x : (i == 1 ? y : z); }
};

struct Quaternion {
    double w{1.0};
    double x{};
    double y{};
    double z{};
};

struct EulerDegrees {
    double yaw{};
    double pitch{};
    double roll{};
};

// The normalized sample every backend produces and the whole tracking pipeline
// operates on. It carries no Bluetooth/Windows state, so recenter, smoothing,
// axis mapping, Euler conversion, and serialisation all work on it independently
// of how it was captured. angularVelocity/acceleration are optional because the
// protocol allows a device to expose orientation without either.
struct MotionSample {
    Vec3 rotationVector{};                   // raw orientation as a rotation vector (backend input)
    Quaternion orientation{};                // filled in by OrientationFilter
    EulerDegrees euler{};                    // filled in by OrientationFilter
    std::optional<Vec3> angularVelocity{};   // gyroscope, radians/second (null when the device omits it)
    std::optional<Vec3> acceleration{};      // accelerometer, m/s^2 (null unless the device reports it)
    std::uint8_t resetCounter{};
    double packetsPerSecond{};
    double receiveLatencyMs{-1.0};
    std::chrono::steady_clock::time_point receivedAt{};
};

struct AxisMapping {
    std::array<unsigned, 3> source{0, 1, 2};
    std::array<double, 3> sign{1.0, 1.0, 1.0};
};

struct FilterConfig {
    double smoothing{0.18};
    double fastMovementRadiansPerSecond{2.5};
    double driftCorrectionPerSecond{0.002};
    // Default convention: YXZ axis order with the X and Z axes inverted. This is
    // the mapping that yields correct head tracking on the WH-1000XM5. Overridable
    // via the CLI (--axis-map / --invert) and the GUI controls.
    AxisMapping axes{{1u, 0u, 2u}, {-1.0, 1.0, -1.0}};
};

} // namespace sony

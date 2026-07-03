// protocol.cpp
// Pure serialisation of a MotionSample. No sockets, no Windows.
#include "sony_head_tracker/protocol.hpp"

#include <format>

namespace sony {

std::array<double, 6> toOpenTrackPose(const MotionSample& s) {
    // Translation is always zero -- this protocol reports orientation only -- so
    // the head angles occupy the last three slots.
    return {0.0, 0.0, 0.0, s.euler.yaw, s.euler.pitch, s.euler.roll};
}

std::string jsonEscapeString(std::string_view utf8) {
    std::string escaped;
    escaped.reserve(utf8.size() + 2);
    escaped.push_back('"');
    for (const char c : utf8) {
        if (c == '"' || c == '\\') escaped.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) escaped.push_back(c);   // control characters are dropped
    }
    escaped.push_back('"');
    return escaped;
}

std::string toJson(const MotionSample& s, std::string_view deviceJson) {
    // gyroscope is radians/second; accelerometer is m/s^2 and is null unless the device actually reports it.
    const auto gyro = s.angularVelocity
        ? std::format("[{:.9g},{:.9g},{:.9g}]", s.angularVelocity->x, s.angularVelocity->y, s.angularVelocity->z)
        : std::string("null");
    const auto accel = s.acceleration
        ? std::format("[{:.9g},{:.9g},{:.9g}]", s.acceleration->x, s.acceleration->y, s.acceleration->z)
        : std::string("null");
    return std::format("{{\"version\":2,\"device\":{},\"rotationVector\":[{:.9g},{:.9g},{:.9g}],\"quaternion\":[{:.9g},{:.9g},{:.9g},{:.9g}],\"yprDegrees\":[{:.9g},{:.9g},{:.9g}],\"gyroscope\":{},\"accelerometer\":{},\"angularVelocity\":{},\"resetCounter\":{},\"packetsPerSecond\":{:.3f},\"receiveLatencyMs\":{:.3f}}}",
        deviceJson, s.rotationVector.x, s.rotationVector.y, s.rotationVector.z,
        s.orientation.w, s.orientation.x, s.orientation.y, s.orientation.z,
        s.euler.yaw, s.euler.pitch, s.euler.roll, gyro, accel, gyro, s.resetCounter, s.packetsPerSecond, s.receiveLatencyMs);
}

} // namespace sony

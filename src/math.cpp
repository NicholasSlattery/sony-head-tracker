// math.cpp
// Pure quaternion / Euler / axis-remap maths. No Windows, no hardware.
#include "sony_head_tracker/math.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sony {

Quaternion normalize(Quaternion q) {
    const auto n = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (n < 1e-12) return {};
    return {q.w/n, q.x/n, q.y/n, q.z/n};
}

Quaternion conjugate(Quaternion q) { return {q.w, -q.x, -q.y, -q.z}; }

Quaternion multiply(Quaternion a, Quaternion b) {
    return normalize({
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
    });
}

Quaternion rotationVectorToQuaternion(Vec3 v) {
    const auto angle = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (angle < 1e-12) return {};
    const auto s = std::sin(angle / 2.0) / angle;
    return normalize({std::cos(angle / 2.0), v.x*s, v.y*s, v.z*s});
}

Vec3 quaternionToRotationVector(Quaternion q) {
    q = normalize(q);
    if (q.w < 0.0) q = {-q.w, -q.x, -q.y, -q.z};
    const auto half = std::acos(std::clamp(q.w, -1.0, 1.0));
    const auto s = std::sin(half);
    if (std::abs(s) < 1e-12) return {};
    const auto factor = 2.0 * half / s;
    return {q.x*factor, q.y*factor, q.z*factor};
}

EulerDegrees quaternionToEulerDegrees(Quaternion q) {
    q = normalize(q);
    // Android head axes: X=right, Y=forward, Z=up. Yaw is around Z.
    const auto sinr = 2.0 * (q.w*q.x + q.y*q.z);
    const auto cosr = 1.0 - 2.0 * (q.x*q.x + q.y*q.y);
    const auto sinp = std::clamp(2.0 * (q.w*q.y - q.z*q.x), -1.0, 1.0);
    const auto siny = 2.0 * (q.w*q.z + q.x*q.y);
    const auto cosy = 1.0 - 2.0 * (q.y*q.y + q.z*q.z);
    constexpr auto d = 180.0 / std::numbers::pi;
    return {std::atan2(siny, cosy)*d, std::asin(sinp)*d, std::atan2(sinr, cosr)*d};
}

Quaternion slerp(Quaternion a, Quaternion b, double t) {
    a = normalize(a); b = normalize(b); t = std::clamp(t, 0.0, 1.0);
    auto dot = a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
    if (dot < 0.0) { b = {-b.w, -b.x, -b.y, -b.z}; dot = -dot; }
    if (dot > 0.9995) return normalize({a.w+t*(b.w-a.w), a.x+t*(b.x-a.x), a.y+t*(b.y-a.y), a.z+t*(b.z-a.z)});
    const auto theta = std::acos(std::clamp(dot, -1.0, 1.0));
    const auto scale = std::sin(theta);
    const auto aa = std::sin((1.0-t)*theta)/scale;
    const auto bb = std::sin(t*theta)/scale;
    return normalize({aa*a.w+bb*b.w, aa*a.x+bb*b.x, aa*a.y+bb*b.y, aa*a.z+bb*b.z});
}

Vec3 remap(Vec3 v, const AxisMapping& m) {
    return {v[m.source[0]]*m.sign[0], v[m.source[1]]*m.sign[1], v[m.source[2]]*m.sign[2]};
}

} // namespace sony

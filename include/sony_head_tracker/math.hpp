// math.hpp
// Pure quaternion / Euler / axis-remap maths. No Windows, no hardware.
#pragma once

#include "sony_head_tracker/types.hpp"

namespace sony {

Quaternion normalize(Quaternion q);
Quaternion conjugate(Quaternion q);
Quaternion multiply(Quaternion a, Quaternion b);
Quaternion rotationVectorToQuaternion(Vec3 vector);
Vec3 quaternionToRotationVector(Quaternion q);
EulerDegrees quaternionToEulerDegrees(Quaternion q);
Quaternion slerp(Quaternion a, Quaternion b, double t);
Vec3 remap(Vec3 value, const AxisMapping& mapping);

} // namespace sony

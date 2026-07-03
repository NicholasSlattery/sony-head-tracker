// math_tests.cpp
// Quaternion <-> Euler, rotation-vector round trips, and axis remap/inversion.
#include "test_framework.hpp"

#include "sony_head_tracker/math.hpp"

using namespace sony;

TEST(identity_quaternion_is_zero_euler) {
    const auto e = quaternionToEulerDegrees(Quaternion{1, 0, 0, 0});
    CHECK_NEAR(e.yaw, 0.0, 1e-9);
    CHECK_NEAR(e.pitch, 0.0, 1e-9);
    CHECK_NEAR(e.roll, 0.0, 1e-9);
}

TEST(ninety_degree_yaw_about_z) {
    // Rotation of 90 degrees about Z: w = cos(45), z = sin(45).
    const double c = 0.70710678118654752440;
    const auto e = quaternionToEulerDegrees(Quaternion{c, 0, 0, c});
    CHECK_NEAR(e.yaw, 90.0, 1e-6);
    CHECK_NEAR(e.pitch, 0.0, 1e-6);
    CHECK_NEAR(e.roll, 0.0, 1e-6);
}

TEST(rotation_vector_round_trip) {
    const Vec3 v{0.1, -0.2, 0.35};
    const auto back = quaternionToRotationVector(rotationVectorToQuaternion(v));
    CHECK_NEAR(back.x, v.x, 1e-9);
    CHECK_NEAR(back.y, v.y, 1e-9);
    CHECK_NEAR(back.z, v.z, 1e-9);
}

TEST(zero_rotation_vector_is_identity) {
    const auto q = rotationVectorToQuaternion(Vec3{0, 0, 0});
    CHECK_NEAR(q.w, 1.0, 1e-12);
    CHECK_NEAR(q.x, 0.0, 1e-12);
    CHECK_NEAR(q.y, 0.0, 1e-12);
    CHECK_NEAR(q.z, 0.0, 1e-12);
}

TEST(remap_identity_is_noop) {
    const auto r = remap(Vec3{1, 2, 3}, AxisMapping{{0, 1, 2}, {1.0, 1.0, 1.0}});
    CHECK_NEAR(r.x, 1.0, 0);
    CHECK_NEAR(r.y, 2.0, 0);
    CHECK_NEAR(r.z, 3.0, 0);
}

TEST(remap_default_yxz_with_inverted_x_and_z) {
    // The WH-1000XM5 default: source {1,0,2}, sign {-1,+1,-1}.
    const auto r = remap(Vec3{1, 2, 3}, AxisMapping{{1, 0, 2}, {-1.0, 1.0, -1.0}});
    CHECK_NEAR(r.x, -2.0, 0);   // -source[1]
    CHECK_NEAR(r.y, 1.0, 0);    // +source[0]
    CHECK_NEAR(r.z, -3.0, 0);   // -source[2]
}

TEST(quaternion_multiply_by_conjugate_is_identity) {
    const auto q = normalize(Quaternion{0.5, 0.5, 0.5, 0.5});
    const auto id = multiply(q, conjugate(q));
    CHECK_NEAR(id.w, 1.0, 1e-9);
    CHECK_NEAR(id.x, 0.0, 1e-9);
    CHECK_NEAR(id.y, 0.0, 1e-9);
    CHECK_NEAR(id.z, 0.0, 1e-9);
}

TEST(slerp_endpoints) {
    const auto a = Quaternion{1, 0, 0, 0};
    const double c = 0.70710678118654752440;
    const auto b = Quaternion{c, 0, 0, c};
    const auto at0 = slerp(a, b, 0.0);
    const auto at1 = slerp(a, b, 1.0);
    CHECK_NEAR(at0.w, a.w, 1e-9);
    CHECK_NEAR(at1.z, b.z, 1e-9);
}

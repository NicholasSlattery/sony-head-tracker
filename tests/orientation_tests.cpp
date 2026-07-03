// orientation_tests.cpp
// OrientationFilter behaviour: recenter, smoothing attenuation, axis mapping, and
// graceful handling of samples with no angular velocity.
#include "test_framework.hpp"

#include "sony_head_tracker/orientation.hpp"

#include <cmath>
#include <numbers>

using namespace sony;

static FilterConfig identityConfig(double smoothing) {
    FilterConfig cfg;
    cfg.axes = AxisMapping{{0, 1, 2}, {1.0, 1.0, 1.0}};   // no remap/inversion
    cfg.smoothing = smoothing;
    cfg.driftCorrectionPerSecond = 0.0;                    // isolate smoothing from drift
    return cfg;
}

static MotionSample rot(Vec3 v) { MotionSample s; s.rotationVector = v; return s; }

TEST(recenter_zeroes_output_for_the_held_pose) {
    OrientationFilter f(identityConfig(1.0));
    f.process(rot({0, 0, 0}));                     // first sample becomes the center
    const auto moved = f.process(rot({0.3, 0, 0}));
    CHECK(std::fabs(moved.euler.roll) > 1.0);      // rotation about X shows up as roll

    f.recenter();
    const auto after = f.process(rot({0.3, 0, 0}));   // recenter onto the current pose
    CHECK_NEAR(after.euler.yaw, 0.0, 1e-6);
    CHECK_NEAR(after.euler.pitch, 0.0, 1e-6);
    CHECK_NEAR(after.euler.roll, 0.0, 1e-6);
}

TEST(smoothing_attenuates_a_single_step) {
    OrientationFilter fast(identityConfig(1.0));   // follows instantly
    OrientationFilter slow(identityConfig(0.1));   // heavily smoothed
    fast.process(rot({0, 0, 0}));
    slow.process(rot({0, 0, 0}));

    const auto fastOut = fast.process(rot({0, 0, 0.5}));   // rotation about Z -> yaw
    const auto slowOut = slow.process(rot({0, 0, 0.5}));

    CHECK(std::fabs(slowOut.euler.yaw) < std::fabs(fastOut.euler.yaw));
    constexpr double deg = 180.0 / std::numbers::pi;
    CHECK_NEAR(fastOut.euler.yaw, 0.5 * deg, 0.5);         // ~28.6 degrees
}

TEST(axis_remap_applies_to_angular_velocity) {
    OrientationFilter f(identityConfig(1.0));
    MotionSample s = rot({0, 0, 0});
    s.angularVelocity = Vec3{1.0, 2.0, 3.0};
    const auto out = f.process(std::move(s));
    CHECK(out.angularVelocity.has_value());
    CHECK_NEAR(out.angularVelocity->x, 1.0, 1e-9);         // identity mapping preserves it
    CHECK_NEAR(out.angularVelocity->y, 2.0, 1e-9);
    CHECK_NEAR(out.angularVelocity->z, 3.0, 1e-9);
}

TEST(missing_angular_velocity_stays_missing_and_is_stable) {
    OrientationFilter f;   // default config
    const auto out = f.process(rot({0.1, 0.1, 0.1}));       // no angularVelocity set
    CHECK(!out.angularVelocity.has_value());                // the filter never fabricates a gyro
    const auto out2 = f.process(rot({0.1, 0.1, 0.1}));
    CHECK(std::isfinite(out2.euler.yaw));
    CHECK(std::isfinite(out2.euler.pitch));
    CHECK(std::isfinite(out2.euler.roll));
}

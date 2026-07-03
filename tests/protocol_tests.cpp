// protocol_tests.cpp
// JSON telemetry + OpenTrack pose serialisation, including the null-gyroscope
// case for devices without angular velocity, and string escaping.
#include "test_framework.hpp"

#include "sony_head_tracker/protocol.hpp"

#include <string>

using namespace sony;

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST(open_track_pose_carries_angles_in_last_three_slots) {
    MotionSample s;
    s.euler = {10.0, 20.0, 30.0};
    const auto pose = toOpenTrackPose(s);
    CHECK_NEAR(pose[0], 0.0, 0);
    CHECK_NEAR(pose[1], 0.0, 0);
    CHECK_NEAR(pose[2], 0.0, 0);
    CHECK_NEAR(pose[3], 10.0, 0);
    CHECK_NEAR(pose[4], 20.0, 0);
    CHECK_NEAR(pose[5], 30.0, 0);
}

TEST(json_reports_null_gyroscope_without_angular_velocity) {
    MotionSample s;   // angularVelocity + acceleration default to nullopt
    const auto json = toJson(s, "null");
    CHECK(contains(json, "\"gyroscope\":null"));
    CHECK(contains(json, "\"accelerometer\":null"));
    CHECK(contains(json, "\"angularVelocity\":null"));
    CHECK(contains(json, "\"device\":null"));
    CHECK(contains(json, "\"version\":2"));
}

TEST(json_includes_gyroscope_array_when_present) {
    MotionSample s;
    s.angularVelocity = Vec3{1.5, -2.0, 0.25};
    const auto json = toJson(s, "null");
    CHECK(contains(json, "\"gyroscope\":[1.5,-2,0.25]"));
    CHECK(!contains(json, "\"gyroscope\":null"));
    CHECK(contains(json, "\"accelerometer\":null"));   // acceleration still absent
}

TEST(json_includes_acceleration_array_when_present) {
    MotionSample s;
    s.acceleration = Vec3{0.0, 9.8, 0.0};
    const auto json = toJson(s, "null");
    CHECK(contains(json, "\"accelerometer\":[0,9.8,0]"));
}

TEST(json_embeds_device_label) {
    MotionSample s;
    const auto json = toJson(s, jsonEscapeString("WH-1000XM5"));
    CHECK(contains(json, "\"device\":\"WH-1000XM5\""));
}

TEST(json_escape_quotes_and_backslashes) {
    CHECK(jsonEscapeString("plain") == "\"plain\"");
    CHECK(jsonEscapeString("a\"b") == "\"a\\\"b\"");
    CHECK(jsonEscapeString("a\\b") == "\"a\\\\b\"");
}

TEST(json_escape_drops_control_characters) {
    // A control byte (0x01) is dropped; surrounding text survives.
    const std::string input = std::string("a") + char(0x01) + "b";
    CHECK(jsonEscapeString(input) == "\"ab\"");
}

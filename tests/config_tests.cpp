// config_tests.cpp
// AppConfig JSON round-trip, tolerant parsing, clamping, and the default-mapping
// check used by the "custom mapping active" indicator.
#include "test_framework.hpp"

#include "sony_head_tracker/app_config.hpp"

using namespace sony;

TEST(config_round_trips_through_json) {
    AppConfig c;
    c.axes = AxisMapping{{2, 0, 1}, {1.0, -1.0, 1.0}};
    c.smoothing = 0.42;
    c.udpPort = 5005;
    c.backend = PreferredBackend::sensor;
    c.showAllDevices = true;
    c.window = {100, 200, 1280, 900};

    const auto parsed = appConfigFromJson(appConfigToJson(c));
    CHECK(parsed.axes.source == c.axes.source);
    CHECK(parsed.axes.sign[0] == c.axes.sign[0]);
    CHECK(parsed.axes.sign[1] == c.axes.sign[1]);
    CHECK(parsed.axes.sign[2] == c.axes.sign[2]);
    CHECK_NEAR(parsed.smoothing, 0.42, 1e-9);
    CHECK(parsed.udpPort == 5005);
    CHECK(parsed.backend == PreferredBackend::sensor);
    CHECK(parsed.showAllDevices == true);
    CHECK(parsed.window.x == 100 && parsed.window.y == 200);
    CHECK(parsed.window.width == 1280 && parsed.window.height == 900);
    CHECK(parsed.window.valid());
}

TEST(missing_keys_keep_defaults) {
    const AppConfig def;
    const auto parsed = appConfigFromJson("{ \"smoothing\": 0.30 }");
    CHECK_NEAR(parsed.smoothing, 0.30, 1e-9);       // provided key applied
    CHECK(parsed.udpPort == def.udpPort);           // everything else default
    CHECK(parsed.axes.source == def.axes.source);
    CHECK(parsed.backend == PreferredBackend::automatic);
    CHECK(!parsed.window.valid());                  // no window persisted
}

TEST(malformed_json_yields_defaults) {
    const AppConfig def;
    const auto parsed = appConfigFromJson("this is not json at all");
    CHECK_NEAR(parsed.smoothing, def.smoothing, 1e-9);
    CHECK(parsed.udpPort == def.udpPort);
    CHECK(parsed.axes.source == def.axes.source);
}

TEST(out_of_range_values_are_clamped_or_ignored) {
    auto parsed = appConfigFromJson("{ \"smoothing\": 9.0, \"udpPort\": 70000 }");
    CHECK_NEAR(parsed.smoothing, 1.0, 1e-9);        // clamped to [0.01, 1.0]
    CHECK(parsed.udpPort == 4242);                  // out of 1..65534 -> default kept

    parsed = appConfigFromJson("{ \"smoothing\": -1.0 }");
    CHECK_NEAR(parsed.smoothing, 0.01, 1e-9);
}

TEST(backend_string_maps_to_enum) {
    CHECK(appConfigFromJson("{\"backend\":\"hid\"}").backend == PreferredBackend::hid);
    CHECK(appConfigFromJson("{\"backend\":\"sensor\"}").backend == PreferredBackend::sensor);
    CHECK(appConfigFromJson("{\"backend\":\"nonsense\"}").backend == PreferredBackend::automatic);
}

TEST(default_axis_mapping_detection) {
    CHECK(isDefaultAxisMapping(AxisMapping{{1, 0, 2}, {-1.0, 1.0, -1.0}}));    // the WH-1000XM5 default
    CHECK(!isDefaultAxisMapping(AxisMapping{{0, 1, 2}, {1.0, 1.0, 1.0}}));     // identity is "custom"
    CHECK(!isDefaultAxisMapping(AxisMapping{{1, 0, 2}, {1.0, 1.0, -1.0}}));    // one sign flipped
}

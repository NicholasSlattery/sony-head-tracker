#include "test_framework.hpp"

#include "sony_head_tracker_c.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

TEST(c_api_rejects_null_handle_and_invalid_ports) {
    CHECK(!sht_start(nullptr, 4242, nullptr, nullptr, nullptr));
    auto* handle = sht_create();
    CHECK(handle != nullptr);
    CHECK(!sht_start(handle, 0, nullptr, nullptr, nullptr));
    CHECK(!sht_start(handle, UINT16_MAX, nullptr, nullptr, nullptr));
    sht_destroy(handle);
}

TEST(c_api_stop_and_destroy_are_idempotent_for_stopped_handle) {
    auto* handle = sht_create();
    CHECK(handle != nullptr);
    sht_stop(handle);
    sht_stop(handle);
    sht_recenter(handle);
    sht_destroy(handle);
    sht_stop(nullptr);
    sht_destroy(nullptr);
}

TEST(c_api_filter_rejects_null_or_invalid_mapping_without_crashing) {
    auto* handle = sht_create();
    CHECK(handle != nullptr);
    const unsigned validSource[3]{1, 0, 2};
    const double validSign[3]{-1.0, 1.0, -1.0};
    const unsigned duplicateSource[3]{0, 0, 2};
    sht_set_filter(handle, 0.18, validSource, validSign);
    sht_set_filter(handle, 0.18, nullptr, validSign);
    sht_set_filter(handle, 0.18, validSource, nullptr);
    sht_set_filter(handle, 0.18, duplicateSource, validSign);
    sht_destroy(handle);
}

TEST(c_api_diagnostics_are_bounded_null_terminated_and_redacted) {
    CHECK(sht_get_diagnostics(nullptr, nullptr, 0) == 0);
    auto* handle = sht_create();
    CHECK(handle != nullptr);
    const auto required = sht_get_diagnostics(handle, nullptr, 0);
    CHECK(required > 1);
    std::vector<char> output(required);
    CHECK(sht_get_diagnostics(handle, output.data(), output.size()) == required);
    CHECK(output.back() == '\0');
    const std::string text(output.data());
    CHECK(text.find("Sony Head Tracker diagnostics") != std::string::npos);
    CHECK(text.find("[verified compatible headset]") != std::string::npos);
    std::array<char, 8> shortOutput{};
    CHECK(sht_get_diagnostics(handle, shortOutput.data(), shortOutput.size()) == required);
    CHECK(shortOutput.back() == '\0');
    sht_destroy(handle);
}

TEST(c_api_config_rejects_null_without_accessing_persistent_state) {
    CHECK(!sht_load_config(nullptr));
    CHECK(!sht_save_config(nullptr));
}

TEST(c_api_config_round_trips_in_an_isolated_application_support_directory) {
    const auto* currentHome = std::getenv("HOME");
    const std::string savedHome = currentHome ? currentHome : "";
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporaryHome = std::filesystem::temp_directory_path() /
        ("sony-head-tracker-config-test-" + std::to_string(unique));
    CHECK(std::filesystem::create_directories(temporaryHome));
    CHECK(setenv("HOME", temporaryHome.c_str(), 1) == 0);

    SHTConfig expected{};
    expected.smoothing = 0.37;
    expected.udp_port = 5252;
    expected.axis_source[0] = 2;
    expected.axis_source[1] = 1;
    expected.axis_source[2] = 0;
    expected.axis_sign[0] = 1;
    expected.axis_sign[1] = -1;
    expected.axis_sign[2] = 1;
    CHECK(sht_save_config(&expected));

    SHTConfig actual{};
    CHECK(sht_load_config(&actual));
    CHECK_NEAR(actual.smoothing, expected.smoothing, 1e-12);
    CHECK(actual.udp_port == expected.udp_port);
    for (std::size_t index = 0; index < 3; ++index) {
        CHECK(actual.axis_source[index] == expected.axis_source[index]);
        CHECK(actual.axis_sign[index] == expected.axis_sign[index]);
    }

    if (savedHome.empty()) unsetenv("HOME");
    else setenv("HOME", savedHome.c_str(), 1);
    std::filesystem::remove_all(temporaryHome);
}

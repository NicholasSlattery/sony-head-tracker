#include "test_framework.hpp"

#include "sony_head_tracker/macos_support.hpp"

#include <array>
#include <chrono>
#include <limits>

using namespace sony;

TEST(macos_report_interval_uses_smallest_nonzero_valid_raw_value) {
    DescriptorField field;
    field.logicalMin = 0;
    field.logicalMax = 63;
    field.physicalMin = 10;
    field.physicalMax = 100;
    field.unitExponent = -3;
    const auto choice = chooseReportInterval(field);
    CHECK(choice.has_value());
    CHECK(choice->raw == 1);
    CHECK(choice->seconds >= 0.010);
    CHECK(choice->seconds <= 0.020);
}

TEST(macos_report_interval_rejects_degenerate_ranges) {
    DescriptorField field;
    CHECK(!chooseReportInterval(field).has_value());
    field.logicalMax = 10;
    field.physicalMin = 10;
    field.physicalMax = 10;
    CHECK(!chooseReportInterval(field).has_value());
}

TEST(macos_verified_device_selection_requires_usage_and_marker) {
    DeviceInfo device;
    device.usagePage = 0x20;
    device.usage = 0xE1;
    CHECK(!isVerifiedAndroidTracker(device));
    device.androidHeadTracker = true;
    CHECK(isVerifiedAndroidTracker(device));
    device.usage = 0xE2;
    CHECK(!isVerifiedAndroidTracker(device));
}

TEST(macos_reconnect_backoff_is_bounded) {
    CHECK(reconnectBackoffSeconds(0) == 1);
    CHECK(reconnectBackoffSeconds(1) == 2);
    CHECK(reconnectBackoffSeconds(2) == 5);
    CHECK(reconnectBackoffSeconds(3) == 10);
    CHECK(reconnectBackoffSeconds(4) == 30);
    CHECK(reconnectBackoffSeconds(1000) == 30);
}

TEST(macos_stream_recovery_never_forces_a_baseband_reconnect) {
    CHECK(streamRecoveryAction(1) == StreamRecoveryAction::refreshServices);
    CHECK(streamRecoveryAction(2) == StreamRecoveryAction::reopenHid);
    CHECK(streamRecoveryAction(1000) == StreamRecoveryAction::reopenHid);
}

TEST(macos_stream_reconnect_backoff_stays_short) {
    CHECK(streamReconnectBackoffSeconds(0) == 1);
    CHECK(streamReconnectBackoffSeconds(1) == 2);
    CHECK(streamReconnectBackoffSeconds(1000) == 2);
}

TEST(macos_reconnect_wait_wakes_on_bluetooth_or_hid_transition) {
    CHECK(trackerAvailabilityBecameReady(false, false, true, false));
    CHECK(trackerAvailabilityBecameReady(false, false, false, true));
    CHECK(trackerAvailabilityBecameReady(true, false, true, true));
    CHECK(!trackerAvailabilityBecameReady(true, true, true, true));
    CHECK(!trackerAvailabilityBecameReady(false, false, false, false));
}

TEST(macos_feature_bits_reject_truncated_and_overflowing_ranges) {
    const FeatureReportLayout noPrefix{0, false};
    const std::array<std::uint8_t, 1> report{0};
    CHECK(!readFeatureBits(report, noPrefix, 7, 2).has_value());
    CHECK(!readFeatureBits(report, noPrefix, std::numeric_limits<std::size_t>::max(), 1).has_value());
    CHECK(!readFeatureBits(report, noPrefix, 0, 0).has_value());
    CHECK(!readFeatureBits(report, noPrefix, 0, 65).has_value());
}

TEST(macos_feature_bits_support_explicit_prefixes_and_exact_boundaries) {
    std::array<std::uint8_t, 2> noPrefix{0, 0};
    const FeatureReportLayout rawLayout{0, false};
    CHECK(writeFeatureBits(noPrefix, rawLayout, 8, 8, 0xA5));
    CHECK(readFeatureBits(noPrefix, rawLayout, 8, 8).value_or(0) == 0xA5);
    CHECK(noPrefix[0] == 0);

    std::array<std::uint8_t, 3> prefixed{7, 0, 0};
    const FeatureReportLayout prefixedLayout{7, true};
    CHECK(writeFeatureBits(prefixed, prefixedLayout, 8, 8, 0x5A));
    CHECK(prefixed[0] == 7);
    CHECK(prefixed[1] == 0);
    CHECK(prefixed[2] == 0x5A);
    CHECK(readFeatureBits(prefixed, prefixedLayout, 8, 8).value_or(0) == 0x5A);
}

TEST(macos_feature_layout_rejects_ambiguous_report_id_metadata) {
    DescriptorField zero{};
    zero.feature = true;
    zero.reportId = 0;
    DescriptorField nonzero{};
    nonzero.feature = true;
    nonzero.reportId = 7;
    const std::array ambiguous{zero, nonzero};
    CHECK(!featureReportLayoutFor(ambiguous, 0).has_value());
    CHECK(!featureReportLayoutFor(ambiguous, 7).has_value());

    const std::array onlyZero{zero};
    const auto noPrefix = featureReportLayoutFor(onlyZero, 0);
    CHECK(noPrefix.has_value());
    CHECK(!noPrefix->hasReportIdPrefix);
}

TEST(macos_feature_write_validation_rejects_stale_or_incomplete_devices_before_writes) {
    DeviceInfo device;
    device.usagePage = 0x20;
    device.usage = 0xE1;
    device.androidHeadTracker = true;
    for (const auto usage : {kPowerFull, std::uint16_t{0x0855},
                             kReportingAllEvents, std::uint16_t{0x0840},
                             kReportInterval}) {
        DescriptorField field;
        field.feature = true;
        field.usagePage = kSensorPage;
        field.usage = usage;
        field.reportId = 0;
        field.reportCount = 1;
        field.bitSize = 1;
        device.fields.push_back(field);
    }
    CHECK(canConfigureFeatureReports(device));

    bool writeCalled = false;
    device.androidHeadTracker = false;
    if (canConfigureFeatureReports(device)) writeCalled = true;
    CHECK(!writeCalled);

    device.androidHeadTracker = true;
    device.fields.pop_back();
    if (canConfigureFeatureReports(device)) writeCalled = true;
    CHECK(!writeCalled);
}

TEST(macos_bluetooth_wait_handles_immediate_delayed_failure_timeout_and_cancellation) {
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{};
    bool connected = true;
    const auto immediate = waitForBluetoothConnection(
        true, [&] { return connected; }, [&](auto) {}, [] { return false; },
        [&] { return now; });
    CHECK(immediate == BluetoothConnectionWaitResult::connected);

    connected = false;
    unsigned pumps{};
    const auto delayed = waitForBluetoothConnection(
        true, [&] { return connected; }, [&](auto duration) {
            now += duration;
            connected = ++pumps == 2;
        }, [] { return false; }, [&] { return now; }, std::chrono::milliseconds(200));
    CHECK(delayed == BluetoothConnectionWaitResult::connected);
    CHECK(pumps == 2);

    const auto failed = waitForBluetoothConnection(
        false, [] { return false; }, [&](auto) {}, [] { return false; }, [&] { return now; });
    CHECK(failed == BluetoothConnectionWaitResult::openFailed);

    const auto timedOut = waitForBluetoothConnection(
        true, [] { return false; }, [&](auto duration) { now += duration; },
        [] { return false; }, [&] { return now; }, std::chrono::milliseconds(50));
    CHECK(timedOut == BluetoothConnectionWaitResult::timedOut);

    const auto cancelled = waitForBluetoothConnection(
        true, [] { return false; }, [&](auto) {}, [] { return true; }, [&] { return now; });
    CHECK(cancelled == BluetoothConnectionWaitResult::cancelled);
    CHECK(bluetoothConnectionConfirmed(immediate, true));
    CHECK(!bluetoothConnectionConfirmed(immediate, false));
    CHECK(!bluetoothConnectionConfirmed(timedOut, true));
}

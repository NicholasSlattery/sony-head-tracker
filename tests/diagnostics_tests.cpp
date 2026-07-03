// diagnostics_tests.cpp
// Redaction of sensitive tokens (Bluetooth addresses, usernames, computer names,
// profile paths, device names) and basic bundle formatting.
#include "test_framework.hpp"

#include "sony_head_tracker/diagnostics.hpp"

#include <string>

using namespace sony;

static bool contains(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

TEST(redacts_mac_addresses) {
    const auto out = redactDiagnostics(L"radio F8:DF:15:AA:BB:CC connected", {});
    CHECK(contains(out, L"[bt-address]"));
    CHECK(!contains(out, L"F8:DF:15:AA:BB:CC"));
}

TEST(redacts_long_hex_runs) {
    // A Bluetooth address embedded in a device instance ID (no separators).
    const auto out = redactDiagnostics(L"BTHENUM\\Dev_F8DF15AABBCC\\7&abc", {});
    CHECK(contains(out, L"[hex]"));
    CHECK(!contains(out, L"F8DF15AABBCC"));
}

TEST(short_hex_is_not_redacted) {
    // 8-hex PnP serial fragments stay (they are not addresses).
    const auto out = redactDiagnostics(L"instance 23A2B970 here", {});
    CHECK(contains(out, L"23A2B970"));
}

TEST(redacts_named_tokens) {
    RedactionTokens t;
    t.username = L"compu";
    t.computerName = L"DESKTOP-1234";
    t.userProfile = L"C:\\Users\\compu";
    t.deviceNames = {L"Nick's WH-1000XM5"};
    const auto out = redactDiagnostics(
        L"path C:\\Users\\compu\\file by compu on DESKTOP-1234 with Nick's WH-1000XM5", t);
    CHECK(contains(out, L"[profile]"));
    CHECK(contains(out, L"[user]"));
    CHECK(contains(out, L"[computer]"));
    CHECK(contains(out, L"[device]"));
    CHECK(!contains(out, L"DESKTOP-1234"));
    CHECK(!contains(out, L"Nick's WH-1000XM5"));
}

TEST(username_substring_does_not_mangle_larger_words) {
    // Regression: username "compu" must not corrupt the word "computer".
    RedactionTokens t;
    t.username = L"compu";
    const auto out = redactDiagnostics(L"the computer belongs to compu", t);
    CHECK(contains(out, L"computer"));      // untouched
    CHECK(contains(out, L"[user]"));        // the standalone token replaced
    CHECK(!contains(out, L"to compu"));
}

TEST(profile_redaction_precedes_username) {
    RedactionTokens t;
    t.username = L"compu";
    t.userProfile = L"C:\\Users\\compu";
    // The profile path should become [profile] as a whole, not "C:\Users\[user]".
    const auto out = redactDiagnostics(L"C:\\Users\\compu\\Desktop", t);
    CHECK(contains(out, L"[profile]"));
    CHECK(!contains(out, L"[user]"));
}

TEST(format_includes_core_fields) {
    DiagnosticsInput in;
    in.appVersion = L"1.3.0";
    in.backend = L"Raw HID";
    in.angularVelocity = true;
    in.logLines = {L"[00:00:00] INFO  hello"};
    const auto text = formatDiagnostics(in);
    CHECK(contains(text, L"Sony Head Tracker diagnostics"));
    CHECK(contains(text, L"1.3.0"));
    CHECK(contains(text, L"Raw HID"));
    CHECK(contains(text, L"Angular velocity:"));
    CHECK(contains(text, L"yes"));
    CHECK(contains(text, L"hello"));
}

// diagnostics.hpp
// Redacted support-bundle generation. Formatting and redaction are pure and
// testable; environment gathering and the CLI entry point live in
// diagnostics_report.cpp.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace sony {

// Tokens scrubbed from a bundle before it leaves the machine.
struct RedactionTokens {
    std::wstring username;
    std::wstring computerName;
    std::wstring userProfile;
    std::vector<std::wstring> deviceNames;   // personalised Bluetooth names
};

struct DiagnosticsInput {
    std::wstring appVersion;
    std::wstring windowsBuild;
    std::wstring backend;            // "Raw HID", "Windows Sensor API", or "none"
    std::wstring headsetModel;       // HID product / sensor friendly name
    std::wstring firmware;           // "unknown" when unavailable
    std::wstring hidUsage;           // e.g. "0x0020:0x00E1"
    std::wstring descriptor;         // sanitised #AndroidHeadTracker# marker
    double packetsPerSecond{-1.0};
    double sampleAgeMs{-1.0};
    bool angularVelocity{false};
    std::wstring destinationPort;    // e.g. "4242 / 4243"
    std::uint64_t udpPacketsSent{0};
    int reconnectionAttempts{0};
    std::wstring settings;           // axis map / invert / smoothing summary
    std::wstring lastError;          // last error log line, or "(none)"
    std::vector<std::wstring> logLines;
};

// --- Pure (diagnostics.cpp) ------------------------------------------------
std::wstring formatDiagnostics(const DiagnosticsInput& input);
std::wstring redactDiagnostics(std::wstring text, const RedactionTokens& tokens);

// --- Windows (diagnostics_report.cpp) --------------------------------------
RedactionTokens currentRedactionTokens();
std::wstring windowsBuildString();
int runDiagnostics(std::wostream& out);   // CLI: enumerate -> gather -> redact -> print

} // namespace sony

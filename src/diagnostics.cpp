// diagnostics.cpp
// Pure formatting + redaction for the support bundle. No Windows.
#include "sony_head_tracker/diagnostics.hpp"

#include <format>
#include <string>
#include <string_view>

namespace sony {

namespace {

bool isHex(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

bool isWordChar(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

// Replaces `from` with `to` only at word boundaries, so a short token like a
// username ("compu") never mangles a larger word that contains it ("computer").
void replaceToken(std::wstring& s, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        const std::size_t end = pos + from.size();
        const bool leftOk = pos == 0 || !isWordChar(s[pos - 1]);
        const bool rightOk = end >= s.size() || !isWordChar(s[end]);
        if (leftOk && rightOk) { s.replace(pos, from.size(), to); pos += to.size(); }
        else ++pos;
    }
}

// Replaces XX:XX:XX:XX:XX:XX Bluetooth addresses with a placeholder.
std::wstring scrubMacAddresses(const std::wstring& s) {
    const auto matches = [&](std::size_t p) {
        for (int g = 0; g < 6; ++g) {
            const std::size_t base = p + static_cast<std::size_t>(g) * 3;
            if (base + 1 >= s.size() || !isHex(s[base]) || !isHex(s[base + 1])) return false;
            if (g < 5 && (base + 2 >= s.size() || s[base + 2] != L':')) return false;
        }
        return true;
    };
    std::wstring out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (matches(i)) { out += L"[bt-address]"; i += 17; }
        else            { out.push_back(s[i]); ++i; }
    }
    return out;
}

// Replaces runs of 12+ hex digits (e.g. a Bluetooth address inside a device
// instance ID) with a placeholder.
std::wstring scrubHexRuns(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (isHex(s[i])) {
            std::size_t j = i;
            while (j < s.size() && isHex(s[j])) ++j;
            if (j - i >= 12) out += L"[hex]";
            else             out.append(s, i, j - i);
            i = j;
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}

} // namespace

std::wstring redactDiagnostics(std::wstring text, const RedactionTokens& t) {
    if (!t.userProfile.empty()) replaceToken(text, t.userProfile, L"[profile]");   // before username (path contains it)
    for (const auto& name : t.deviceNames) if (name.size() > 2) replaceToken(text, name, L"[device]");
    if (t.username.size() > 1) replaceToken(text, t.username, L"[user]");
    if (t.computerName.size() > 1) replaceToken(text, t.computerName, L"[computer]");
    text = scrubMacAddresses(text);
    text = scrubHexRuns(text);
    return text;
}

std::wstring formatDiagnostics(const DiagnosticsInput& in) {
    std::wstring out;
    out += L"Sony Head Tracker diagnostics\n";
    out += L"=============================\n";
    out += L"Redacted before saving: Bluetooth addresses, Windows username, computer name,\n";
    out += L"and known device names. Please review once more before sharing publicly.\n\n";
    const auto line = [&](std::wstring_view label, std::wstring_view value) {
        out += std::format(L"{:<20}{}\n", label, value);
    };
    line(L"App version:", in.appVersion);
    line(L"Windows build:", in.windowsBuild);
    line(L"Selected backend:", in.backend);
    line(L"Headset model:", in.headsetModel.empty() ? L"(none)" : in.headsetModel);
    line(L"Firmware:", in.firmware.empty() ? L"unknown" : in.firmware);
    line(L"HID usage:", in.hidUsage.empty() ? L"(n/a)" : in.hidUsage);
    line(L"Descriptor:", in.descriptor.empty() ? L"(n/a)" : in.descriptor);
    line(L"Packet rate:", in.packetsPerSecond < 0 ? std::wstring(L"n/a") : std::format(L"{:.1f} /s", in.packetsPerSecond));
    line(L"Sample age:", in.sampleAgeMs < 0 ? std::wstring(L"n/a") : std::format(L"{:.1f} ms", in.sampleAgeMs));
    line(L"Angular velocity:", in.angularVelocity ? L"yes" : L"no");
    line(L"UDP destination:", std::format(L"127.0.0.1:{}", in.destinationPort));
    line(L"UDP packets sent:", std::format(L"{}", in.udpPacketsSent));
    line(L"Reconnect attempts:", std::format(L"{}", in.reconnectionAttempts));
    line(L"Last error:", in.lastError.empty() ? L"(none)" : in.lastError);
    out += L"\nSettings:\n";
    out += in.settings;
    if (!in.settings.empty() && in.settings.back() != L'\n') out += L'\n';
    out += L"\nRecent log:\n";
    for (const auto& l : in.logLines) { out += L"  "; out += l; out += L'\n'; }
    return out;
}

} // namespace sony

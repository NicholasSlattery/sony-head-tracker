// app_config.cpp
// Pure serialisation of AppConfig to/from a small, flat JSON object. Tolerant
// parser: missing/unknown keys keep defaults. No Windows.
#include "sony_head_tracker/app_config.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace sony {

namespace {

// Returns the text immediately following the ':' after an exact "key" (quotes
// included), with leading whitespace skipped. Empty view when the key is absent.
std::string_view valueAfterKey(std::string_view j, std::string_view quotedKey) {
    const auto pos = j.find(quotedKey);
    if (pos == std::string_view::npos) return {};
    const auto colon = j.find(':', pos + quotedKey.size());
    if (colon == std::string_view::npos) return {};
    auto v = j.substr(colon + 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\n' || v.front() == '\r')) v.remove_prefix(1);
    return v;
}

std::optional<double> findNumber(std::string_view j, std::string_view key) {
    const auto v = valueAfterKey(j, key);
    if (v.empty()) return std::nullopt;
    double out{};
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    if (r.ec != std::errc{}) return std::nullopt;
    return out;
}

std::optional<bool> findBool(std::string_view j, std::string_view key) {
    const auto v = valueAfterKey(j, key);
    if (v.starts_with("true")) return true;
    if (v.starts_with("false")) return false;
    return std::nullopt;
}

std::optional<std::string> findString(std::string_view j, std::string_view key) {
    const auto v = valueAfterKey(j, key);
    if (v.empty() || v.front() != '"') return std::nullopt;
    std::string out;
    for (std::size_t i = 1; i < v.size(); ++i) {
        const char c = v[i];
        if (c == '\\' && i + 1 < v.size()) { out.push_back(v[i + 1]); ++i; continue; }
        if (c == '"') return out;
        out.push_back(c);
    }
    return std::nullopt;
}

std::vector<double> findNumberArray(std::string_view j, std::string_view key) {
    std::vector<double> out;
    const auto v = valueAfterKey(j, key);
    if (v.empty() || v.front() != '[') return out;
    std::size_t i = 1;
    while (i < v.size() && v[i] != ']') {
        while (i < v.size() && (v[i] == ' ' || v[i] == ',' || v[i] == '\t' || v[i] == '\n' || v[i] == '\r')) ++i;
        if (i >= v.size() || v[i] == ']') break;
        double num{};
        const auto r = std::from_chars(v.data() + i, v.data() + v.size(), num);
        if (r.ec != std::errc{}) break;
        out.push_back(num);
        i = static_cast<std::size_t>(r.ptr - v.data());
    }
    return out;
}

} // namespace

bool isDefaultAxisMapping(const AxisMapping& a) {
    const AxisMapping d{{1u, 0u, 2u}, {-1.0, 1.0, -1.0}};
    return a.source == d.source && a.sign[0] == d.sign[0] && a.sign[1] == d.sign[1] && a.sign[2] == d.sign[2];
}

std::string appConfigToJson(const AppConfig& c) {
    const char* backend = c.backend == PreferredBackend::hid ? "hid" : c.backend == PreferredBackend::sensor ? "sensor" : "auto";
    return std::format(
        "{{\n"
        "  \"axisSource\": [{},{},{}],\n"
        "  \"axisSign\": [{:.0f},{:.0f},{:.0f}],\n"
        "  \"smoothing\": {:.4g},\n"
        "  \"udpPort\": {},\n"
        "  \"backend\": \"{}\",\n"
        "  \"showAllDevices\": {},\n"
        "  \"windowX\": {}, \"windowY\": {}, \"windowWidth\": {}, \"windowHeight\": {}\n"
        "}}\n",
        c.axes.source[0], c.axes.source[1], c.axes.source[2],
        c.axes.sign[0], c.axes.sign[1], c.axes.sign[2],
        c.smoothing, c.udpPort, backend, c.showAllDevices ? "true" : "false",
        c.window.x, c.window.y, c.window.width, c.window.height);
}

AppConfig appConfigFromJson(std::string_view j) {
    AppConfig c;
    if (const auto src = findNumberArray(j, "\"axisSource\""); src.size() == 3) {
        for (int i = 0; i < 3; ++i) { const int s = static_cast<int>(src[i]); if (s >= 0 && s < 3) c.axes.source[i] = static_cast<unsigned>(s); }
    }
    if (const auto sign = findNumberArray(j, "\"axisSign\""); sign.size() == 3) {
        for (int i = 0; i < 3; ++i) c.axes.sign[i] = sign[i] < 0 ? -1.0 : 1.0;
    }
    if (const auto n = findNumber(j, "\"smoothing\"")) c.smoothing = std::clamp(*n, 0.01, 1.0);
    if (const auto n = findNumber(j, "\"udpPort\"")) { if (*n >= 1 && *n <= 65534) c.udpPort = static_cast<std::uint16_t>(*n); }
    if (const auto s = findString(j, "\"backend\"")) {
        c.backend = *s == "hid" ? PreferredBackend::hid : *s == "sensor" ? PreferredBackend::sensor : PreferredBackend::automatic;
    }
    if (const auto b = findBool(j, "\"showAllDevices\"")) c.showAllDevices = *b;
    if (const auto n = findNumber(j, "\"windowX\"")) c.window.x = static_cast<int>(*n);
    if (const auto n = findNumber(j, "\"windowY\"")) c.window.y = static_cast<int>(*n);
    if (const auto n = findNumber(j, "\"windowWidth\"")) c.window.width = static_cast<int>(*n);
    if (const auto n = findNumber(j, "\"windowHeight\"")) c.window.height = static_cast<int>(*n);
    return c;
}

} // namespace sony

// app_config.hpp
// Persisted GUI settings. Serialisation (to/from JSON) is pure and testable; the
// load/save/import/export helpers are the Windows-side persistence and live in
// app_config_store.cpp.
#pragma once

#include "sony_head_tracker/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace sony {

enum class PreferredBackend { automatic, hid, sensor };

struct WindowPlacement {
    int x{-1};
    int y{-1};
    int width{-1};
    int height{-1};
    [[nodiscard]] bool valid() const { return width > 0 && height > 0; }
};

struct AppConfig {
    // Default convention: YXZ order with X and Z inverted (the WH-1000XM5 mapping).
    AxisMapping axes{{1u, 0u, 2u}, {-1.0, 1.0, -1.0}};
    double smoothing{0.18};
    std::uint16_t udpPort{4242};
    PreferredBackend backend{PreferredBackend::automatic};
    bool showAllDevices{false};
    WindowPlacement window{};
};

// True when the axis mapping matches the built-in default (used to surface a
// "custom mapping active" indicator in the GUI).
bool isDefaultAxisMapping(const AxisMapping& axes);

// --- Pure serialisation (app_config.cpp) -----------------------------------
std::string appConfigToJson(const AppConfig& config);
// Tolerant: unknown or missing keys keep their default; malformed input yields
// the default-constructed config.
AppConfig appConfigFromJson(std::string_view json);

// --- Windows persistence (app_config_store.cpp) ----------------------------
std::wstring appConfigPath();                                     // %LOCALAPPDATA%\SonyHeadTracker\config.json
AppConfig loadAppConfig();                                        // reads appConfigPath(); defaults if absent/unreadable
bool saveAppConfig(const AppConfig& config);                     // creates the folder and writes appConfigPath()
bool exportAppConfig(const AppConfig& config, const std::wstring& path);
bool importAppConfig(AppConfig& config, const std::wstring& path);

} // namespace sony

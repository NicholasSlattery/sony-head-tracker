// audio_wake.hpp
// Narrow macOS boundary for activating the selected headset's Bluetooth audio
// transport with zero-filled PCM for the lifetime of a tracking session.
#pragma once

#include <memory>
#include <string_view>

namespace sony {

class SilentAudioWake {
public:
    SilentAudioWake();
    ~SilentAudioWake();

    SilentAudioWake(const SilentAudioWake&) = delete;
    SilentAudioWake& operator=(const SilentAudioWake&) = delete;

    bool start(std::wstring_view productName,
               std::wstring_view bluetoothAddress);
    void stop();
    [[nodiscard]] bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sony

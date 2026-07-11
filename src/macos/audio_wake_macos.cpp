// audio_wake_macos.cpp
// Starts a zero-filled AUHAL output stream on the exact Bluetooth headset
// audio device. This keeps A2DP active without changing the system default
// output or producing audible media.
#include "sony_head_tracker/audio_wake.hpp"

#include "sony_head_tracker/logger.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace sony {
namespace {

template <typename T>
bool readProperty(AudioObjectID object,
                  AudioObjectPropertySelector selector,
                  AudioObjectPropertyScope scope,
                  T& value) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) == noErr &&
           size == sizeof(value);
}

CFStringRef copyStringProperty(AudioObjectID object,
                               AudioObjectPropertySelector selector) {
    CFStringRef value{};
    if (!readProperty(object, selector, kAudioObjectPropertyScopeGlobal, value)) return nullptr;
    return value;
}

std::wstring wideString(CFStringRef value) {
    if (!value) return {};
    const auto length = CFStringGetLength(value);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (length != 0) {
        CFStringGetBytes(value, CFRangeMake(0, length), kCFStringEncodingUTF32LE,
                         0, false, reinterpret_cast<UInt8*>(result.data()),
                         static_cast<CFIndex>(result.size() * sizeof(wchar_t)), nullptr);
    }
    return result;
}

std::wstring stringProperty(AudioObjectID object,
                            AudioObjectPropertySelector selector) {
    auto value = copyStringProperty(object, selector);
    if (!value) return {};
    auto result = wideString(value);
    CFRelease(value);
    return result;
}

std::string normalizedHex(std::wstring_view value) {
    std::string result;
    for (const auto character : value) {
        if (character >= L'0' && character <= L'9') {
            result.push_back(static_cast<char>(character));
        } else if (character >= L'a' && character <= L'f') {
            result.push_back(static_cast<char>(character - L'a' + 'A'));
        } else if (character >= L'A' && character <= L'F') {
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

bool equalCaseInsensitive(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(),
                      [](wchar_t lhs, wchar_t rhs) {
                          return std::towlower(lhs) == std::towlower(rhs);
                      });
}

UInt32 outputChannelCount(AudioDeviceID device) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration,
                                       kAudioDevicePropertyScopeOutput,
                                       kAudioObjectPropertyElementMain};
    UInt32 size{};
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr ||
        size < offsetof(AudioBufferList, mBuffers)) {
        return 0;
    }
    std::vector<std::byte> storage(size);
    auto* buffers = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, buffers) != noErr) {
        return 0;
    }
    UInt32 channels{};
    for (UInt32 index = 0; index < buffers->mNumberBuffers; ++index) {
        channels += buffers->mBuffers[index].mNumberChannels;
    }
    return channels;
}

struct AudioCandidate {
    AudioDeviceID id{};
    std::wstring name;
    std::wstring uid;
    UInt32 channels{};
};

std::vector<AudioCandidate> bluetoothOutputCandidates() {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size{};
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address,
                                       0, nullptr, &size) != noErr) {
        return {};
    }
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                   &size, devices.data()) != noErr) {
        return {};
    }

    std::vector<AudioCandidate> candidates;
    for (const auto device : devices) {
        UInt32 transport{};
        if (!readProperty(device, kAudioDevicePropertyTransportType,
                          kAudioObjectPropertyScopeGlobal, transport) ||
            (transport != kAudioDeviceTransportTypeBluetooth &&
             transport != kAudioDeviceTransportTypeBluetoothLE)) {
            continue;
        }
        auto name = stringProperty(device, kAudioObjectPropertyName);
        const auto channels = outputChannelCount(device);
        if (channels == 0) continue;
        candidates.push_back({device, std::move(name),
                              stringProperty(device, kAudioDevicePropertyDeviceUID),
                              channels});
    }
    return candidates;
}

AudioDeviceID selectDevice(std::wstring_view productName,
                           std::wstring_view bluetoothAddress,
                           UInt32& channels) {
    const auto candidates = bluetoothOutputCandidates();
    if (candidates.empty()) {
        Logger::instance().write(
            LogLevel::warning,
            L"No Bluetooth CoreAudio output is currently available");
        return kAudioObjectUnknown;
    }

    const auto address = normalizedHex(bluetoothAddress);
    if (address.size() == 12) {
        const AudioCandidate* addressMatch{};
        for (const auto& candidate : candidates) {
            if (normalizedHex(candidate.uid).find(address) == std::string::npos) continue;
            if (addressMatch) {
                Logger::instance().write(
                    LogLevel::warning,
                    L"Multiple CoreAudio outputs match the verified Bluetooth address");
                return kAudioObjectUnknown;
            }
            addressMatch = &candidate;
        }
        if (addressMatch) {
            channels = addressMatch->channels;
            return addressMatch->id;
        }
    }

    const AudioCandidate* nameMatch{};
    for (const auto& candidate : candidates) {
        if (!equalCaseInsensitive(candidate.name, productName)) continue;
        if (nameMatch) {
            Logger::instance().write(
                LogLevel::warning,
                std::format(L"Multiple Bluetooth CoreAudio outputs are named '{}'; refusing an ambiguous audio wake",
                            productName));
            return kAudioObjectUnknown;
        }
        nameMatch = &candidate;
    }
    if (!nameMatch) {
        Logger::instance().write(
            LogLevel::warning,
            std::format(L"No Bluetooth CoreAudio output matches the verified address or product name '{}'",
                        productName));
        return kAudioObjectUnknown;
    }
    channels = nameMatch->channels;
    return nameMatch->id;
}

OSStatus renderSilence(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                       UInt32, UInt32, AudioBufferList* output) {
    if (!output) return noErr;
    for (UInt32 index = 0; index < output->mNumberBuffers; ++index) {
        auto& buffer = output->mBuffers[index];
        if (buffer.mData && buffer.mDataByteSize != 0) {
            std::memset(buffer.mData, 0, buffer.mDataByteSize);
        }
    }
    // Deliberately do not set kAudioUnitRenderAction_OutputIsSilence. Some
    // Bluetooth drivers can optimize an explicitly silent stream without
    // activating A2DP; the PCM data itself remains entirely zero-filled.
    return noErr;
}

void logAudioError(std::wstring_view operation, OSStatus status) {
    Logger::instance().write(
        LogLevel::warning,
        std::format(L"Silent audio wake {} failed with CoreAudio status {}",
                    operation, static_cast<std::int32_t>(status)));
}

} // namespace

struct SilentAudioWake::Impl {
    AudioUnit unit{};
    bool running{};
};

SilentAudioWake::SilentAudioWake() : impl_(std::make_unique<Impl>()) {}
SilentAudioWake::~SilentAudioWake() { stop(); }

bool SilentAudioWake::start(std::wstring_view productName,
                            std::wstring_view bluetoothAddress) {
    stop();
    UInt32 deviceChannels{};
    const auto device = selectDevice(productName, bluetoothAddress, deviceChannels);
    if (device == kAudioObjectUnknown) return false;

    AudioComponentDescription description{
        kAudioUnitType_Output,
        kAudioUnitSubType_HALOutput,
        kAudioUnitManufacturer_Apple,
        0,
        0,
    };
    const auto component = AudioComponentFindNext(nullptr, &description);
    if (!component) {
        Logger::instance().write(LogLevel::warning,
                                 L"CoreAudio HAL output component is unavailable");
        return false;
    }
    auto status = AudioComponentInstanceNew(component, &impl_->unit);
    if (status != noErr) {
        logAudioError(L"component creation", status);
        stop();
        return false;
    }
    status = AudioUnitSetProperty(impl_->unit, kAudioOutputUnitProperty_CurrentDevice,
                                  kAudioUnitScope_Global, 0,
                                  &device, sizeof(device));
    if (status != noErr) {
        logAudioError(L"device selection", status);
        stop();
        return false;
    }

    Float64 sampleRate{48000.0};
    readProperty(device, kAudioDevicePropertyNominalSampleRate,
                 kAudioObjectPropertyScopeGlobal, sampleRate);
    AudioStreamBasicDescription format{};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagIsFloat) |
                          static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeEndian) |
                          static_cast<AudioFormatFlags>(kAudioFormatFlagIsPacked) |
                          static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);
    format.mBytesPerPacket = sizeof(Float32);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(Float32);
    format.mChannelsPerFrame = std::clamp<UInt32>(deviceChannels, 1, 2);
    format.mBitsPerChannel = 8 * sizeof(Float32);
    status = AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0,
                                  &format, sizeof(format));
    if (status != noErr) {
        logAudioError(L"stream format setup", status);
        stop();
        return false;
    }

    AURenderCallbackStruct callback{renderSilence, nullptr};
    status = AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0,
                                  &callback, sizeof(callback));
    if (status != noErr) {
        logAudioError(L"render callback setup", status);
        stop();
        return false;
    }
    status = AudioUnitInitialize(impl_->unit);
    if (status != noErr) {
        logAudioError(L"initialization", status);
        stop();
        return false;
    }
    status = AudioOutputUnitStart(impl_->unit);
    if (status != noErr) {
        logAudioError(L"start", status);
        stop();
        return false;
    }
    impl_->running = true;
    Logger::instance().write(
        LogLevel::info,
        std::format(L"Silent A2DP keepalive stream started on '{}'", productName));
    return true;
}

void SilentAudioWake::stop() {
    if (!impl_ || !impl_->unit) return;
    if (impl_->running) AudioOutputUnitStop(impl_->unit);
    AudioUnitUninitialize(impl_->unit);
    AudioComponentInstanceDispose(impl_->unit);
    impl_->unit = nullptr;
    impl_->running = false;
}

bool SilentAudioWake::active() const {
    return impl_ && impl_->running;
}

} // namespace sony

// output_udp.hpp
// UDP transport: ships the pure protocol serialisation (OpenTrack doubles on the
// chosen port, JSON telemetry on port+1) over loopback.
#pragma once

#ifdef _WIN32
#include "sony_head_tracker/windows_prelude.hpp"
#endif

#include "sony_head_tracker/types.hpp"

#include <cstdint>
#ifndef _WIN32
#include <memory>
#endif
#include <string>
#include <string_view>

namespace sony {

class UdpOutput {
public:
    UdpOutput();
    ~UdpOutput();
    UdpOutput(const UdpOutput&) = delete;
    UdpOutput& operator=(const UdpOutput&) = delete;
    bool open(std::string host, std::uint16_t port);
    void setDeviceLabel(std::wstring_view name);   // headset name for the JSON "device" field
    void send(const MotionSample& sample);
    void close();
#ifdef _WIN32
    [[nodiscard]] std::uint64_t packetsSent() const { return packetsSent_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
#else
    [[nodiscard]] std::uint64_t packetsSent() const;
    [[nodiscard]] std::uint16_t port() const;
#endif

private:
#ifdef _WIN32
    SOCKET socket_{INVALID_SOCKET};
    sockaddr_in destination_{};
    sockaddr_in jsonDestination_{};
    std::string deviceJson_{"null"};
    std::string jsonBuffer_;
    std::uint64_t packetsSent_{};
    std::uint16_t port_{};
#else
    struct Context;
    std::unique_ptr<Context> context_;
#endif
};

} // namespace sony

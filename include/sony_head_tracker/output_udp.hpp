// output_udp.hpp
// UDP transport: ships the pure protocol serialisation (OpenTrack doubles on the
// chosen port, JSON telemetry on port+1) over loopback. Winsock types appear in
// the private state, so this is a platform header.
#pragma once

#include "sony_head_tracker/windows_prelude.hpp"

#include "sony_head_tracker/types.hpp"

#include <cstdint>
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
    [[nodiscard]] std::uint64_t packetsSent() const { return packetsSent_; }   // one per send() (OpenTrack + JSON pair)
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    SOCKET socket_{INVALID_SOCKET};
    sockaddr_in destination_{};
    std::string deviceJson_{"null"};
    std::uint64_t packetsSent_{};
    std::uint16_t port_{};
};

} // namespace sony

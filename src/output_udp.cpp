// output_udp.cpp
// UDP transport for the OpenTrack + JSON datagrams. Serialisation itself is pure
// (protocol.hpp); this file only owns the socket.
#include "sony_head_tracker/windows_prelude.hpp"

#include "sony_head_tracker/output_udp.hpp"

#include "sony_head_tracker/protocol.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace sony {

UdpOutput::UdpOutput() { WSADATA data{}; WSAStartup(MAKEWORD(2,2), &data); }
UdpOutput::~UdpOutput() { close(); WSACleanup(); }

bool UdpOutput::open(std::string host, std::uint16_t port) {
    close();
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) return false;
    destination_.sin_family = AF_INET;
    destination_.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &destination_.sin_addr) != 1) { close(); return false; }
    port_ = port;
    return true;
}

void UdpOutput::setDeviceLabel(std::wstring_view name) {
    if (name.empty()) { deviceJson_ = "null"; return; }
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes > 0 ? static_cast<std::size_t>(bytes) : 0, '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), utf8.data(), bytes, nullptr, nullptr);
    deviceJson_ = jsonEscapeString(utf8);
}

void UdpOutput::send(const MotionSample& s) {
    if (socket_ == INVALID_SOCKET) return;
    // OpenTrack's UDP input expects six native doubles in pose order.
    const auto openTrack = toOpenTrackPose(s);
    sendto(socket_, reinterpret_cast<const char*>(openTrack.data()), static_cast<int>(openTrack.size() * sizeof(double)), 0,
           reinterpret_cast<const sockaddr*>(&destination_), sizeof(destination_));
    // The JSON datagram follows immediately on port+1 so consumers never need packet sniffing to distinguish formats.
    const auto json = toJson(s, deviceJson_);
    auto jsonDest = destination_; jsonDest.sin_port = htons(static_cast<u_short>(ntohs(destination_.sin_port) + 1));
    sendto(socket_, json.data(), static_cast<int>(json.size()), 0, reinterpret_cast<const sockaddr*>(&jsonDest), sizeof(jsonDest));
    ++packetsSent_;
}

void UdpOutput::close() { if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; } }

} // namespace sony

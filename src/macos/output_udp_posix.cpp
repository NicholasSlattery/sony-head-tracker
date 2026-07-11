// output_udp_posix.cpp
// POSIX UDP transport for the OpenTrack + JSON loopback datagrams.
#include "sony_head_tracker/output_udp.hpp"

#include "sony_head_tracker/protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bit>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace sony {

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(double) == 8);

namespace {

void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
        codePoint = 0xFFFD;
    }
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

std::string wideToUtf8(std::wstring_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(input[index]);
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 1 < input.size()) {
            const auto low = static_cast<std::uint32_t>(input[index + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
        }
        appendUtf8(output, codePoint);
    }
    return output;
}

} // namespace

struct UdpOutput::Context {
    int socket{-1};
    sockaddr_in destination{};
    sockaddr_in jsonDestination{};
    std::string deviceJson{"null"};
    std::string jsonBuffer;
    std::atomic_uint64_t packetsSent{};
    std::uint16_t port{};
};

UdpOutput::UdpOutput() : context_(std::make_unique<Context>()) {}

UdpOutput::~UdpOutput() { close(); }

bool UdpOutput::open(std::string host, std::uint16_t port) {
    close();
    if (port == 0 || port == 65535) return false;
    context_->socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (context_->socket < 0) return false;
    context_->destination = {};
    context_->destination.sin_family = AF_INET;
    context_->destination.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &context_->destination.sin_addr) != 1) {
        close();
        return false;
    }
    context_->jsonDestination = context_->destination;
    context_->jsonDestination.sin_port = htons(static_cast<std::uint16_t>(port + 1));
    context_->port = port;
    return true;
}

void UdpOutput::setDeviceLabel(std::wstring_view name) {
    context_->deviceJson = name.empty() ? "null" : jsonEscapeString(wideToUtf8(name));
}

void UdpOutput::send(const MotionSample& sample) {
    if (context_->socket < 0) return;
    const auto openTrack = toOpenTrackPose(sample);
    sendto(context_->socket, openTrack.data(), openTrack.size() * sizeof(double), 0,
           reinterpret_cast<const sockaddr*>(&context_->destination),
           sizeof(context_->destination));
    toJsonTo(context_->jsonBuffer, sample, context_->deviceJson);
    sendto(context_->socket, context_->jsonBuffer.data(), context_->jsonBuffer.size(), 0,
           reinterpret_cast<const sockaddr*>(&context_->jsonDestination),
           sizeof(context_->jsonDestination));
    context_->packetsSent.fetch_add(1, std::memory_order_relaxed);
}

void UdpOutput::close() {
    if (context_ && context_->socket >= 0) {
        ::close(context_->socket);
        context_->socket = -1;
    }
}

std::uint64_t UdpOutput::packetsSent() const {
    return context_->packetsSent.load(std::memory_order_relaxed);
}
std::uint16_t UdpOutput::port() const { return context_->port; }

} // namespace sony

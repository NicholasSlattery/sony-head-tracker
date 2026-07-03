// protocol.hpp
// Pure serialisation of a MotionSample to the two wire formats: OpenTrack's six
// native doubles and the JSON telemetry datagram. No sockets, no Windows -- the
// UDP backend just ships whatever these produce. See docs/PROTOCOL.md.
#pragma once

#include "sony_head_tracker/types.hpp"

#include <array>
#include <string>
#include <string_view>

namespace sony {

// OpenTrack pose order: x, y, z (translation, always zero here) then yaw, pitch,
// roll in degrees.
std::array<double, 6> toOpenTrackPose(const MotionSample& sample);

// Returns a quoted, JSON-escaped string literal for `utf8` (control characters
// are dropped, matching the historic device-label behaviour).
std::string jsonEscapeString(std::string_view utf8);

// Serialises the JSON telemetry datagram. `deviceJson` must already be a valid
// JSON value -- either "null" or a quoted string from jsonEscapeString().
std::string toJson(const MotionSample& sample, std::string_view deviceJson);

} // namespace sony

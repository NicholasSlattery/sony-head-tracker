// hid_usages.hpp
// HID sensor-page usage IDs for the Android Head Tracker profile, plus the
// verification marker. Kept as plain std::uint16_t (not the Windows USAGE
// typedef) so both the platform HID backend and pure code can share them.
#pragma once

#include <cstdint>
#include <string_view>

namespace sony {

inline constexpr std::uint16_t kSensorPage = 0x20;
inline constexpr std::uint16_t kOtherCustom = 0xE1;
inline constexpr std::uint16_t kSensorDescription = 0x0308;
inline constexpr std::uint16_t kReportInterval = 0x030E;
inline constexpr std::uint16_t kReportingAllEvents = 0x0841;
inline constexpr std::uint16_t kPowerFull = 0x0851;
inline constexpr std::uint16_t kTransportAcl = 0xF800;
// Android Head Tracker custom data fields (HID sensor page 0x20).
inline constexpr std::uint16_t kRotation = 0x0544;        // orientation rotation vector
inline constexpr std::uint16_t kAngularVelocity = 0x0545; // gyroscope (rad/s), vector form
inline constexpr std::uint16_t kResetCounter = 0x0546;
// Standard HID sensor-page motion fields, parsed opportunistically so that any
// firmware which also reports raw inertial data has it surfaced. Sony's
// Android Head Tracker profile normally exposes orientation + gyro only.
inline constexpr std::uint16_t kAccelerationVector = 0x0452; // acceleration, vector form
inline constexpr std::uint16_t kAccelerationX = 0x0453;      // acceleration about X (m/s^2)
inline constexpr std::uint16_t kAccelerationY = 0x0454;
inline constexpr std::uint16_t kAccelerationZ = 0x0455;
inline constexpr std::uint16_t kAngularVelocityVector = 0x0456; // angular velocity, vector form
inline constexpr std::uint16_t kAngularVelocityX = 0x0457;      // angular velocity about X (rad/s)
inline constexpr std::uint16_t kAngularVelocityY = 0x0458;
inline constexpr std::uint16_t kAngularVelocityZ = 0x0459;

inline constexpr std::string_view kMarker = "#AndroidHeadTracker#";

} // namespace sony

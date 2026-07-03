// hid_descriptor.hpp
// Pure HID report-descriptor value decoding: unit-exponent decode, logical->
// physical scaling, signed bitfield extraction from a packed value array, and
// the raw signed-integer helper used by the descriptor dumper. No Windows.
#pragma once

#include "sony_head_tracker/device.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace sony {

double descriptorScale(std::int64_t raw, std::int32_t logicalMin, std::int32_t logicalMax,
                       std::int32_t physicalMin, std::int32_t physicalMax, std::int8_t unitExponent);
std::int8_t decodeHidUnitExponent(std::uint32_t exponent);
std::vector<double> decodePackedDescriptorValues(std::span<const std::uint8_t> packed, const DescriptorField& field);
// Sign-extends the low `bytes*8` bits of `value` (HID descriptor items store
// signed minimums/maximums as variable-width little-endian integers).
std::int64_t hidSigned(std::uint32_t value, unsigned bytes);

} // namespace sony

// descriptor_tests.cpp
// HID descriptor value decoding: unit-exponent decode, logical->physical scaling,
// signed bitfield extraction, and robustness against truncated packed buffers.
#include "test_framework.hpp"

#include "sony_head_tracker/hid_descriptor.hpp"

#include <array>

using namespace sony;

static DescriptorField field(std::uint16_t bitSize, std::uint16_t count, std::int32_t lmin, std::int32_t lmax,
                             std::int32_t pmin = 0, std::int32_t pmax = 0, std::int8_t exp = 0) {
    DescriptorField f;
    f.bitSize = bitSize; f.reportCount = count;
    f.logicalMin = lmin; f.logicalMax = lmax; f.physicalMin = pmin; f.physicalMax = pmax; f.unitExponent = exp;
    return f;
}

TEST(unit_exponent_decode) {
    CHECK(decodeHidUnitExponent(0x00) == 0);
    CHECK(decodeHidUnitExponent(0x0F) == -1);   // 0xF -> -1
    CHECK(decodeHidUnitExponent(0x08) == -8);   // 0x8 -> -8
    CHECK(decodeHidUnitExponent(0x07) == 7);
}

TEST(descriptor_scale_linear) {
    // raw 100 mapped from [0,1000] onto [0,10] -> 1.0
    CHECK_NEAR(descriptorScale(100, 0, 1000, 0, 10, 0), 1.0, 1e-12);
    // same, scaled by 10^-2
    CHECK_NEAR(descriptorScale(100, 0, 1000, 0, 10, -2), 0.01, 1e-12);
}

TEST(descriptor_scale_passthrough_when_no_physical_range) {
    CHECK_NEAR(descriptorScale(42, 0, 0, 0, 0, 0), 42.0, 0);
    CHECK_NEAR(descriptorScale(-7, -100, 100, 0, 0, 0), -7.0, 0);
}

TEST(signed_bitfield_extraction) {
    // 16-bit signed, all bits set -> -1
    const std::array<std::uint8_t, 2> allOnes{0xFF, 0xFF};
    auto r = decodePackedDescriptorValues(allOnes, field(16, 1, -32768, 32767));
    CHECK(r.size() == 1);
    CHECK_NEAR(r[0], -1.0, 0);

    // 16-bit signed, 0x8000 (little-endian) -> -32768
    const std::array<std::uint8_t, 2> minVal{0x00, 0x80};
    r = decodePackedDescriptorValues(minVal, field(16, 1, -32768, 32767));
    CHECK_NEAR(r[0], -32768.0, 0);
}

TEST(unsigned_multi_value_extraction) {
    // Two 8-bit unsigned values packed back to back.
    const std::array<std::uint8_t, 2> bytes{0x01, 0x02};
    const auto r = decodePackedDescriptorValues(bytes, field(8, 2, 0, 255));
    CHECK(r.size() == 2);
    CHECK_NEAR(r[0], 1.0, 0);
    CHECK_NEAR(r[1], 2.0, 0);
}

TEST(truncated_buffer_does_not_read_out_of_bounds) {
    // Field claims 16 bits but only one byte is present: high bits read as zero.
    const std::array<std::uint8_t, 1> oneByte{0xFF};
    const auto r = decodePackedDescriptorValues(oneByte, field(16, 1, -32768, 32767));
    CHECK(r.size() == 1);
    CHECK_NEAR(r[0], 255.0, 0);   // 0x00FF, sign bit (15) absent -> positive 255
}

TEST(empty_buffer_yields_zeroes_not_crash) {
    const std::array<std::uint8_t, 0> empty{};
    const auto r = decodePackedDescriptorValues(empty, field(16, 3, 0, 65535));
    CHECK(r.size() == 3);
    for (double v : r) CHECK_NEAR(v, 0.0, 0);
}

TEST(degenerate_bit_size_returns_empty) {
    const std::array<std::uint8_t, 8> bytes{};
    CHECK(decodePackedDescriptorValues(bytes, field(0, 1, 0, 1)).empty());    // zero bit size
    CHECK(decodePackedDescriptorValues(bytes, field(64, 1, 0, 1)).empty());   // > 63 bits
}

TEST(hid_signed_variable_width) {
    CHECK(hidSigned(0xFF, 1) == -1);
    CHECK(hidSigned(0x80, 1) == -128);
    CHECK(hidSigned(0x7F, 1) == 127);
    CHECK(hidSigned(0xFFFF, 2) == -1);
    CHECK(hidSigned(0x1234, 4) == 0x1234);
    CHECK(hidSigned(0, 0) == 0);
}

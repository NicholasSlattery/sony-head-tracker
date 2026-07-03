// hid_descriptor.cpp
// Pure HID report-descriptor value decoding. No Windows.
#include "sony_head_tracker/hid_descriptor.hpp"

#include <cmath>

namespace sony {

double descriptorScale(std::int64_t raw, std::int32_t lmin, std::int32_t lmax,
                       std::int32_t pmin, std::int32_t pmax, std::int8_t exponent) {
    if (lmax == lmin || (pmax == 0 && pmin == 0)) return static_cast<double>(raw);
    const auto fraction = (static_cast<double>(raw) - lmin) / (static_cast<double>(lmax) - lmin);
    return (pmin + fraction * (static_cast<double>(pmax) - pmin)) * std::pow(10.0, exponent);
}

std::int8_t decodeHidUnitExponent(std::uint32_t exponent) {
    auto nibble = static_cast<std::int8_t>(exponent & 0x0f);
    return nibble >= 8 ? static_cast<std::int8_t>(nibble - 16) : nibble;
}

std::vector<double> decodePackedDescriptorValues(std::span<const std::uint8_t> packed, const DescriptorField& field) {
    std::vector<double> result;
    if (!field.bitSize || field.bitSize > 63) return result;
    result.reserve(field.reportCount);
    for (unsigned valueIndex=0; valueIndex<field.reportCount; ++valueIndex) {
        std::uint64_t raw{};
        const auto offset=static_cast<std::size_t>(valueIndex)*field.bitSize;
        for (unsigned bitIndex=0; bitIndex<field.bitSize; ++bitIndex) {
            const auto bit=offset+bitIndex;
            if (bit/8 < packed.size() && (packed[bit/8] & (1u << (bit%8)))) raw |= std::uint64_t{1} << bitIndex;
        }
        std::int64_t value=static_cast<std::int64_t>(raw);
        if (field.logicalMin < 0) {
            const auto sign=std::uint64_t{1} << (field.bitSize-1);
            const auto mask=(std::uint64_t{1} << field.bitSize)-1;
            value=static_cast<std::int64_t>(((raw&mask)^sign)-sign);
        }
        result.push_back(descriptorScale(value,field.logicalMin,field.logicalMax,field.physicalMin,field.physicalMax,field.unitExponent));
    }
    return result;
}

std::int64_t hidSigned(std::uint32_t value, unsigned bytes) {
    if (bytes==0) return 0;
    const auto bits=bytes*8u;
    if (bits>=32) return static_cast<std::int32_t>(value);
    const auto sign=std::uint32_t{1}<<(bits-1);
    const auto mask=(std::uint32_t{1}<<bits)-1;
    value&=mask;
    return static_cast<std::int32_t>((value^sign)-sign);
}

} // namespace sony

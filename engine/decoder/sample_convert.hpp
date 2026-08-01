#pragma once

// Source-format -> normalised float conversion table (M02). No clamping, no dithering: the decoded
// buffer must be a faithful representation of the file (values above +-1.0 are real headroom
// information that M08/M11 need).

#include <cstddef>
#include <cstdint>
#include <span>

#include "../util/audio_types.hpp"

namespace aud::decoder {

inline constexpr Sample convertU8(std::uint8_t x) noexcept {
    return (static_cast<float>(x) - 128.0f) / 128.0f;
}

inline constexpr Sample convertS16(std::int16_t x) noexcept {
    return static_cast<float>(x) / 32768.0f;  // divide by 32768, not 32767 — keeps 0 at 0
}

// `raw` holds the low 24 bits of a signed 24-bit sample in a 32-bit int (not yet sign-extended).
inline constexpr Sample convertS24(std::int32_t raw) noexcept {
    std::int32_t signExtended = raw;
    if (signExtended & 0x00800000) {
        signExtended |= static_cast<std::int32_t>(0xFF000000);
    }
    return static_cast<float>(signExtended) / 8388608.0f;
}

inline constexpr Sample convertS32(std::int32_t x) noexcept {
    return static_cast<float>(x) / 2147483648.0f;
}

inline constexpr Sample convertF64(double x) noexcept { return static_cast<float>(x); }

// Bulk, SIMD-friendly conversions. `srcBytes` is tightly packed source samples; `out` is the
// destination span (already sized to the frame count).
void convertBufferU8(std::span<const std::uint8_t> src, std::span<Sample> out) noexcept;
void convertBufferS16(std::span<const std::int16_t> src, std::span<Sample> out) noexcept;
void convertBufferS24Packed(std::span<const std::byte> src3BytesPerSample, std::span<Sample> out) noexcept;
void convertBufferS32(std::span<const std::int32_t> src, std::span<Sample> out) noexcept;
void convertBufferF64(std::span<const double> src, std::span<Sample> out) noexcept;

}  // namespace aud::decoder

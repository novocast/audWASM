#include "sample_convert.hpp"

#include <cstring>

namespace aud::decoder {

void convertBufferU8(std::span<const std::uint8_t> src, std::span<Sample> out) noexcept {
    const std::size_t n = src.size() < out.size() ? src.size() : out.size();
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = convertU8(src[i]);
    }
}

void convertBufferS16(std::span<const std::int16_t> src, std::span<Sample> out) noexcept {
    const std::size_t n = src.size() < out.size() ? src.size() : out.size();
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = convertS16(src[i]);
    }
}

void convertBufferS24Packed(std::span<const std::byte> src3BytesPerSample, std::span<Sample> out) noexcept {
    const std::size_t n = (src3BytesPerSample.size() / 3) < out.size() ? (src3BytesPerSample.size() / 3) : out.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::byte* p = src3BytesPerSample.data() + (i * 3);
        // Little-endian 24-bit, as produced by essentially every format we support.
        const auto raw = static_cast<std::int32_t>(static_cast<std::uint8_t>(p[0])) |
                          (static_cast<std::int32_t>(static_cast<std::uint8_t>(p[1])) << 8) |
                          (static_cast<std::int32_t>(static_cast<std::uint8_t>(p[2])) << 16);
        out[i] = convertS24(raw);
    }
}

void convertBufferS32(std::span<const std::int32_t> src, std::span<Sample> out) noexcept {
    const std::size_t n = src.size() < out.size() ? src.size() : out.size();
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = convertS32(src[i]);
    }
}

void convertBufferF64(std::span<const double> src, std::span<Sample> out) noexcept {
    const std::size_t n = src.size() < out.size() ? src.size() : out.size();
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = convertF64(src[i]);
    }
}

}  // namespace aud::decoder

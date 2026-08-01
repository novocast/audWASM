#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "../../engine/decoder/sample_convert.hpp"

using namespace aud::decoder;

TEST_CASE("u8 conversion keeps 128 at 0 and covers the full range", "[sample_convert]") {
    REQUIRE(convertU8(128) == 0.0f);
    REQUIRE(convertU8(0) == -1.0f);
    REQUIRE(convertU8(255) == Catch::Approx(0.9921875f));
}

TEST_CASE("s16 conversion divides by 32768, not 32767", "[sample_convert]") {
    REQUIRE(convertS16(0) == 0.0f);
    REQUIRE(convertS16(32767) == Catch::Approx(32767.0f / 32768.0f));
    REQUIRE(convertS16(-32768) == -1.0f);
}

TEST_CASE("s24 conversion sign-extends correctly", "[sample_convert]") {
    REQUIRE(convertS24(0x000000) == 0.0f);
    REQUIRE(convertS24(0x7FFFFF) == Catch::Approx(8388607.0f / 8388608.0f));
    // -1 as a 24-bit two's complement value is 0xFFFFFF (low 24 bits).
    REQUIRE(convertS24(0x00FFFFFF) == Catch::Approx(-1.0f / 8388608.0f));
    // The most negative 24-bit value: 0x800000 -> -8388608 -> -1.0f exactly.
    REQUIRE(convertS24(0x00800000) == -1.0f);
}

TEST_CASE("s32 conversion divides by 2^31", "[sample_convert]") {
    REQUIRE(convertS32(0) == 0.0f);
    REQUIRE(convertS32(-2147483647 - 1) == -1.0f);
}

TEST_CASE("f64 conversion narrows without altering in-range values", "[sample_convert]") {
    REQUIRE(convertF64(0.5) == Catch::Approx(0.5f));
    REQUIRE(convertF64(-1.25) == Catch::Approx(-1.25f));  // exceeding +-1 is preserved, not clamped
}

TEST_CASE("bulk conversions match the scalar conversions element-wise", "[sample_convert]") {
    const std::vector<std::int16_t> src{0, 32767, -32768, 100, -100};
    std::vector<aud::Sample>        out(src.size());
    convertBufferS16(src, out);
    for (std::size_t i = 0; i < src.size(); ++i) {
        REQUIRE(out[i] == convertS16(src[i]));
    }
}

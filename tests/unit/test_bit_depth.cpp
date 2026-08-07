#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "../../engine/analysis/statistics/bit_depth.hpp"

using Catch::Approx;
using aud::Sample;
using aud::statistics::BitDepthAccumulator;
using aud::statistics::detectEffectiveBitDepth;

namespace {

// Builds a normalised float buffer whose values are exact multiples of 2^-(depth-1) — i.e.
// genuine `depth`-bit content, with no dither.
std::vector<Sample> makeQuantisedTone(std::uint32_t depth, std::size_t n) {
    const double        scale = static_cast<double>(1ull << (depth - 1));
    std::vector<Sample> out(n);
    std::mt19937                                rng(12345);
    std::uniform_int_distribution<std::int64_t> dist(-static_cast<std::int64_t>(scale), static_cast<std::int64_t>(scale) - 1);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<Sample>(static_cast<double>(dist(rng)) / scale);
    }
    return out;
}

}  // namespace

TEST_CASE("bit_depth: 16-bit content padded into a 24-bit container reports effective depth 16", "[statistics]") {
    // Every sample is an exact multiple of 2^(24-16) = 2^8 in the 24-bit integer domain.
    auto samples16 = makeQuantisedTone(16, 20000);

    auto result = detectEffectiveBitDepth(samples16, 24);
    REQUIRE(result.effectiveBitDepth.has_value());
    CHECK(*result.effectiveBitDepth == 16);
    CHECK(result.containerBitDepth == 24);
    CHECK(result.describe() == "24-bit container, 16-bit effective content");
}

TEST_CASE("bit_depth: genuine 24-bit content reports effective depth 24 (no free bits)", "[statistics]") {
    auto samples24 = makeQuantisedTone(24, 20000);

    auto result = detectEffectiveBitDepth(samples24, 24);
    REQUIRE(result.effectiveBitDepth.has_value());
    CHECK(*result.effectiveBitDepth == 24);
}

TEST_CASE("bit_depth: 8/12/20-bit content in a 24-bit container is each detected correctly", "[statistics]") {
    for (std::uint32_t depth : {8u, 12u, 20u}) {
        auto samples = makeQuantisedTone(depth, 20000);
        auto result  = detectEffectiveBitDepth(samples, 24);
        REQUIRE(result.effectiveBitDepth.has_value());
        CHECK(*result.effectiveBitDepth == depth);
    }
}

TEST_CASE("bit_depth: streaming across multiple process() calls matches a single call", "[statistics]") {
    auto samples = makeQuantisedTone(16, 20000);

    BitDepthAccumulator streaming;
    streaming.begin(24);
    streaming.process(std::span<const Sample>(samples).first(7000));
    streaming.process(std::span<const Sample>(samples).subspan(7000));
    auto streamingResult = streaming.finish();

    auto batchResult = detectEffectiveBitDepth(samples, 24);

    REQUIRE(streamingResult.effectiveBitDepth.has_value());
    REQUIRE(batchResult.effectiveBitDepth.has_value());
    CHECK(*streamingResult.effectiveBitDepth == *batchResult.effectiveBitDepth);
}

TEST_CASE("bit_depth: a full-resolution (k=0) LSB-random signal is hedged as probable dither", "[statistics]") {
    // Full-scale white noise at 16-bit resolution populates every bit, including a pseudo-random
    // LSB — the same signature a well-dithered signal has.
    std::mt19937                                rng(999);
    std::uniform_int_distribution<std::int32_t> dist(-32768, 32767);
    std::vector<Sample>              samples(50000);
    for (auto& s : samples) s = static_cast<Sample>(static_cast<double>(dist(rng)) / 32768.0);

    auto result = detectEffectiveBitDepth(samples, 16);
    REQUIRE(result.effectiveBitDepth.has_value());
    CHECK(*result.effectiveBitDepth == 16);
    CHECK(result.ditherLikely);
    CHECK(result.ditherConfidence > 0.0);
}

TEST_CASE("bit_depth: content with unused low bits is never flagged as dither", "[statistics]") {
    // k > 0 (there are free low bits) means the dither question doesn't even apply — the
    // heuristic only looks at LSB behaviour when every bit down to bit 0 is populated (k == 0).
    // 8-bit content in a 16-bit container leaves k = 8, regardless of what those 8 unused bits'
    // LSB-equivalent would have looked like.
    auto samples = makeQuantisedTone(8, 20000);

    auto result = detectEffectiveBitDepth(samples, 16);
    REQUIRE(result.effectiveBitDepth.has_value());
    CHECK(*result.effectiveBitDepth == 8);
    CHECK_FALSE(result.ditherLikely);
}

TEST_CASE("bit_depth: float source with a 16-bit quantisation grid is detected", "[statistics]") {
    auto samples = makeQuantisedTone(16, 20000);  // already on the 16-bit grid, containerBitDepth=0 (float)

    auto result = detectEffectiveBitDepth(samples, 0);
    CHECK(result.containerBitDepth == 0);
    REQUIRE(result.effectiveBitDepth.has_value());
    CHECK(*result.effectiveBitDepth == 16);
}

TEST_CASE("bit_depth: float source with no quantisation grid reports none detected", "[statistics]") {
    std::mt19937                          rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<Sample>                    samples(20000);
    for (auto& s : samples) s = dist(rng);

    auto result = detectEffectiveBitDepth(samples, 0);
    CHECK_FALSE(result.effectiveBitDepth.has_value());
    CHECK(result.describe() == "float, no quantisation detected");
}

TEST_CASE("bit_depth: pure silence reports the container depth with no dither claim", "[statistics]") {
    std::vector<Sample> silence(1000, 0.0f);
    auto                 result = detectEffectiveBitDepth(silence, 16);
    REQUIRE(result.effectiveBitDepth.has_value());
    CHECK(*result.effectiveBitDepth == 16);
    CHECK_FALSE(result.ditherLikely);
}

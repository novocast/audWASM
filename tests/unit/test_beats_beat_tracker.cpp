#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "../../engine/analysis/beats/beat_tracker.hpp"

using Catch::Approx;
using aud::beats::BeatTrackerConfig;
using aud::beats::trackBeats;

namespace {
constexpr double kHopSeconds = 512.0 / 44100.0;

std::vector<float> periodicOdf(double periodSeconds, double firstBeatSeconds, std::size_t frameCount) {
    std::vector<float> odf(frameCount, 0.0f);
    const double        periodFrames = periodSeconds / kHopSeconds;
    for (double center = firstBeatSeconds / kHopSeconds; center < static_cast<double>(frameCount);
         center += periodFrames) {
        for (std::size_t i = 0; i < frameCount; ++i) {
            const double d = static_cast<double>(i) - center;
            if (std::fabs(d) < 6.0) odf[i] += static_cast<float>(std::exp(-0.5 * d * d));
        }
    }
    return odf;
}
}  // namespace

TEST_CASE("beats::trackBeats: beats land on the ODF peaks of a regular pulse train, with high phase confidence",
          "[beats][beat_tracker]") {
    const auto odf    = periodicOdf(0.5, 0.0, 2000);
    const auto result = trackBeats(odf, kHopSeconds, 0.5, BeatTrackerConfig{});

    REQUIRE(result.beatFrames.size() > 5);
    CHECK(result.phaseConfidence > 0.3f);

    const double periodFrames = 0.5 / kHopSeconds;
    for (std::size_t i = 1; i < result.beatFrames.size(); ++i) {
        const double interval = static_cast<double>(result.beatFrames[i] - result.beatFrames[i - 1]);
        CHECK(interval == Approx(periodFrames).margin(1.0));
    }
}

TEST_CASE("beats::trackBeats: an offset pulse train (first beat at 0.25s) is tracked with the correct phase",
          "[beats][beat_tracker]") {
    const auto odf    = periodicOdf(0.5, 0.25, 2000);
    const auto result = trackBeats(odf, kHopSeconds, 0.5, BeatTrackerConfig{});

    REQUIRE_FALSE(result.beatFrames.empty());
    const double firstBeatSeconds = static_cast<double>(result.beatFrames.front()) * kHopSeconds;

    // The first tracked beat should land near 0.25s (mod the 0.5s period — the DP may anchor to
    // any beat in the grid, not necessarily the very first one in time).
    const double phaseError = std::fmod(firstBeatSeconds - 0.25 + 10.5, 0.5) - 0.25;
    CHECK(std::fabs(phaseError) < 0.05);
}

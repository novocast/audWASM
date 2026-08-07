#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "../../engine/analysis/loudness/gating.hpp"
#include "../../engine/analysis/loudness/lra.hpp"

using Catch::Approx;
using aud::loudness::computeLoudnessRange;
using aud::loudness::meanSquareFromLoudness;

TEST_CASE("lra: constant loudness has zero range", "[loudness]") {
    std::vector<double> blocks(60, meanSquareFromLoudness(-23.0));
    CHECK(computeLoudnessRange(blocks) == Approx(0.0).margin(1e-6));
}

TEST_CASE("lra: silence produces NaN, not 0", "[loudness]") {
    std::vector<double> blocks(20, 0.0);
    CHECK(std::isnan(computeLoudnessRange(blocks)));

    std::vector<double> empty;
    CHECK(std::isnan(computeLoudnessRange(empty)));
}

TEST_CASE("lra: matches hand-computed P95-P10 for an evenly spaced, ungated distribution", "[loudness]") {
    // 11 short-term values evenly spaced 1 LU apart from -25 to -15 LUFS. The spread (10 LU) is
    // narrow enough that Tech 3342's relative gate (-20 LU off the power-weighted mean, which
    // for this narrow a spread sits close to the loudest end) never trips, so every block
    // survives and the result is a direct nearest-rank percentile calculation — matching Tech
    // 3342 §5's own published MATLAB reference exactly: round((n-1)*p/100 + 1) in 1-based MATLAB
    // indexing, i.e. round((n-1)*p/100) here (0-based):
    //   index for P10  = round(0.10*(11-1)) = round(1.0) = 1   -> -24 LUFS
    //   index for P95  = round(0.95*(11-1)) = round(9.5) = 10  -> -15 LUFS
    //   LRA = -15 - (-24) = 9.0 LU
    std::vector<double> blocks;
    for (int i = 0; i < 11; ++i) {
        blocks.push_back(meanSquareFromLoudness(-25.0 + static_cast<double>(i)));
    }
    CHECK(computeLoudnessRange(blocks) == Approx(9.0).margin(0.05));
}

TEST_CASE("lra: relative gate removes a quiet minority the same way the integrated gate does, "
          "just at a -20 LU offset instead of -10",
          "[loudness]") {
    // 90% of blocks at -23 LUFS, 10% at -50 LUFS (well above the -70 absolute gate, but far more
    // than 20 LU below the power-weighted mean once combined) -> the quiet minority should be
    // gated out, leaving a uniform set and therefore zero range.
    std::vector<double> blocks(90, meanSquareFromLoudness(-23.0));
    blocks.insert(blocks.end(), 10, meanSquareFromLoudness(-50.0));
    CHECK(computeLoudnessRange(blocks) == Approx(0.0).margin(1e-3));
}

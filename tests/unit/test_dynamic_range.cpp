#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "../../engine/analysis/statistics/dynamic_range.hpp"

using Catch::Approx;
using aud::statistics::computeDynamicRangeDb;

TEST_CASE("dynamic_range: empty input reports 0", "[statistics]") {
    CHECK(computeDynamicRangeDb({}, {}) == 0.0);
}

TEST_CASE("dynamic_range: uniform blocks with a fixed crest factor reproduce that crest factor", "[statistics]") {
    // Every block has the same RMS and peak, so DR must equal the per-block crest factor exactly
    // regardless of which 20% get selected.
    constexpr double        rms  = 0.1;
    constexpr double        peak = 0.5;  // 14 dB crest
    std::vector<double>     blockRms(10, rms);
    std::vector<double>     blockPeaks(10, peak);

    const double dr = computeDynamicRangeDb(blockRms, blockPeaks);
    CHECK(dr == Approx(20.0 * std::log10(peak / rms)).margin(1e-9));
}

TEST_CASE("dynamic_range: only the loudest 20% of blocks (by RMS) are used", "[statistics]") {
    // 10 blocks: one loud, quiet-peaked block and nine quiet, loud-peaked blocks. Only the top 20%
    // (the single loudest-RMS block) should be selected — DR should reflect *that* block's crest
    // factor, not the other nine's.
    std::vector<double> blockRms  = {0.9, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1};
    std::vector<double> blockPeaks = {0.91, 0.99, 0.99, 0.99, 0.99, 0.99, 0.99, 0.99, 0.99, 0.99};

    const double dr = computeDynamicRangeDb(blockRms, blockPeaks);
    // 20% of 10 blocks = 2 blocks would be selected in a strict top-20%, but with a single clear
    // outlier and nine identical runners-up, either the top-1 or top-2 selection is dominated by
    // the loud block's much smaller crest factor (0.91/0.9 ~ 0.1dB) rather than the quiet blocks'
    // (0.99/0.1 ~ 19.9dB) — assert it lands near the loud block's figure, not the quiet ones'.
    CHECK(dr < 5.0);
}

TEST_CASE("dynamic_range: a single block degenerates to that block's crest factor", "[statistics]") {
    const double dr = computeDynamicRangeDb({0.2}, {0.8});
    CHECK(dr == Approx(20.0 * std::log10(0.8 / 0.2)).margin(1e-9));
}

TEST_CASE("dynamic_range: silent blocks (zero RMS) don't crash and sort last", "[statistics]") {
    std::vector<double> blockRms  = {0.0, 0.0, 0.3, 0.3, 0.3};
    std::vector<double> blockPeaks = {0.0, 0.0, 0.9, 0.9, 0.9};

    const double dr = computeDynamicRangeDb(blockRms, blockPeaks);
    CHECK(dr == Approx(20.0 * std::log10(0.9 / 0.3)).margin(1e-6));
}

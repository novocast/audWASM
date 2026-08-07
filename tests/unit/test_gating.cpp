#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "../../engine/analysis/loudness/gating.hpp"

using Catch::Approx;
using aud::loudness::gateIntegratedLoudness;
using aud::loudness::loudnessFromMeanSquare;
using aud::loudness::meanSquareFromLoudness;

TEST_CASE("gating: loudness/mean-square round-trip is exact", "[loudness]") {
    for (double lufs : {-70.0, -40.0, -23.0, -14.0, -3.0, 0.0}) {
        const double z = meanSquareFromLoudness(lufs);
        CHECK(loudnessFromMeanSquare(z) == Approx(lufs).margin(1e-9));
    }
}

TEST_CASE("gating: silence (z=0) is -infinity, never 0", "[loudness]") {
    CHECK(std::isinf(loudnessFromMeanSquare(0.0)));
    CHECK(loudnessFromMeanSquare(0.0) < 0.0);
}

TEST_CASE("gating: uniform loudness blocks integrate to exactly that loudness", "[loudness]") {
    const double z = meanSquareFromLoudness(-23.0);
    std::vector<double> blocks(100, z);
    auto result = gateIntegratedLoudness(blocks);
    CHECK(result.integratedLufs == Approx(-23.0).margin(1e-6));
}

TEST_CASE("gating: pure silence produces NaN, not 0", "[loudness]") {
    std::vector<double> blocks(50, 0.0);
    auto result = gateIntegratedLoudness(blocks);
    CHECK(std::isnan(result.integratedLufs));

    std::vector<double> empty;
    auto emptyResult = gateIntegratedLoudness(empty);
    CHECK(std::isnan(emptyResult.integratedLufs));
}

TEST_CASE("gating: absolute gate discards blocks below -70 LUFS", "[loudness]") {
    const double loud  = meanSquareFromLoudness(-20.0);
    const double quiet = meanSquareFromLoudness(-80.0);  // below the absolute gate entirely

    std::vector<double> blocks(100, loud);
    blocks.insert(blocks.end(), 100, quiet);

    auto result = gateIntegratedLoudness(blocks);
    // Only the loud blocks should survive either gate -> integrated equals the loud level exactly.
    CHECK(result.integratedLufs == Approx(-20.0).margin(1e-6));
}

TEST_CASE("gating: relative gate discards quiet-but-above-absolute-threshold blocks", "[loudness]") {
    // Half the blocks at -20 LUFS, half at -50 LUFS. -50 is comfortably above the -70 absolute
    // gate but, once the two groups are power-averaged, more than 10 LU below that average —
    // the relative gate should remove it, leaving only the -20 LUFS group.
    const double loud  = meanSquareFromLoudness(-20.0);
    const double quiet = meanSquareFromLoudness(-50.0);

    std::vector<double> blocks(50, loud);
    blocks.insert(blocks.end(), 50, quiet);

    auto result = gateIntegratedLoudness(blocks);
    CHECK(result.integratedLufs == Approx(-20.0).margin(1e-3));
}

TEST_CASE("gating: relative gate is a no-op when all surviving blocks are close in level", "[loudness]") {
    // Two groups within a few LU of each other should both survive the relative gate; the
    // integrated value should land between them, not collapse to just one group.
    const double a = meanSquareFromLoudness(-22.0);
    const double b = meanSquareFromLoudness(-24.0);

    std::vector<double> blocks(50, a);
    blocks.insert(blocks.end(), 50, b);

    auto result = gateIntegratedLoudness(blocks);
    CHECK(result.integratedLufs > -24.0);
    CHECK(result.integratedLufs < -22.0);
}

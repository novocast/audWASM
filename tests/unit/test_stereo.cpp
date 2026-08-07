#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "../../engine/analysis/statistics/accumulator.hpp"
#include "../../engine/analysis/statistics/stereo.hpp"

using Catch::Approx;
using aud::Sample;
using aud::statistics::ChannelAccumulator;
using aud::statistics::StereoAccumulator;
using aud::statistics::StereoStatistics;

// Runs the full stereo pipeline (two ChannelAccumulators + StereoAccumulator) exactly as
// StatisticsAnalyzer wires them together, so these tests exercise the real computeStatistics()
// call, not a hand-rolled formula.
StereoStatistics runStereoPipeline(const std::vector<Sample>& left, const std::vector<Sample>& right,
                                    aud::SampleRate sampleRate = 44100) {
    ChannelAccumulator accL, accR;
    accL.begin(sampleRate);
    accR.begin(sampleRate);
    accL.process(left, 0);
    accR.process(right, 0);
    accL.finish();
    accR.finish();

    StereoAccumulator stereo;
    stereo.begin(sampleRate);
    stereo.process(left, right);
    stereo.finish();

    return stereo.computeStatistics(accL.mean(), accL.variance(), accL.sumXSquares(), accR.mean(), accR.variance(),
                                     accR.sumXSquares());
}

TEST_CASE("stereo: identical channels correlate exactly 1.0", "[statistics]") {
    std::mt19937                          rng(1);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<Sample>                    signal(20000);
    for (auto& s : signal) s = dist(rng);

    auto stats = runStereoPipeline(signal, signal);
    CHECK(stats.correlation == Approx(1.0).margin(1e-9));
    CHECK(stats.balanceDb == Approx(0.0).margin(1e-9));
    CHECK(stats.monoCompatibilityDb == Approx(0.0).margin(1e-6));
}

TEST_CASE("stereo: inverted channels correlate exactly -1.0 with mono compatibility -inf", "[statistics]") {
    std::mt19937                          rng(2);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<Sample>                    left(20000);
    for (auto& s : left) s = dist(rng);
    std::vector<Sample> right(left.size());
    for (std::size_t i = 0; i < left.size(); ++i) right[i] = -left[i];

    auto stats = runStereoPipeline(left, right);
    CHECK(stats.correlation == Approx(-1.0).margin(1e-9));
    CHECK(stats.monoCompatibilityDb == -std::numeric_limits<double>::infinity());
}

TEST_CASE("stereo: independent white noise channels correlate close to 0", "[statistics]") {
    std::mt19937                          rngL(3), rngR(4);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    constexpr std::size_t                  n = 200000;
    std::vector<Sample>                    left(n), right(n);
    for (auto& s : left) s = dist(rngL);
    for (auto& s : right) s = dist(rngR);

    auto stats = runStereoPipeline(left, right);
    CHECK(std::fabs(stats.correlation) < 0.02);
}

TEST_CASE("stereo: balance reports the level difference between channels", "[statistics]") {
    std::vector<Sample> left(10000, 0.5f);
    std::vector<Sample> right(10000, 0.25f);  // -6.02 dB relative to left

    auto stats = runStereoPipeline(left, right);
    CHECK(stats.balanceDb == Approx(20.0 * std::log10(0.25 / 0.5)).margin(1e-6));
}

TEST_CASE("stereo: correlation series localises a phase-inverted section in time", "[statistics]") {
    constexpr aud::SampleRate sampleRate = 44100;
    constexpr std::size_t     n          = sampleRate * 10;  // 10 seconds
    std::mt19937                          rng(7);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<Sample> left(n);
    for (auto& s : left) s = dist(rng);
    std::vector<Sample> right = left;

    // Invert a 4-second section starting at t=3s.
    const std::size_t invertStart = sampleRate * 3;
    const std::size_t invertEnd   = sampleRate * 7;
    for (std::size_t i = invertStart; i < invertEnd; ++i) right[i] = -right[i];

    auto stats = runStereoPipeline(left, right, sampleRate);

    const std::size_t windowFrames = sampleRate / 20;  // 50ms
    const std::size_t invertStartWindow = invertStart / windowFrames;
    const std::size_t invertEndWindow   = invertEnd / windowFrames;

    // Windows well inside the inverted section read strongly negative; windows well outside it
    // (and before/after with margin for window-boundary straddling) read strongly positive.
    for (std::size_t w = invertStartWindow + 2; w < invertEndWindow - 2; ++w) {
        REQUIRE(w < stats.correlationSeries.size());
        CHECK(stats.correlationSeries[w] < -0.9f);
    }
    for (std::size_t w = 2; w < invertStartWindow - 2; ++w) {
        CHECK(stats.correlationSeries[w] > 0.9f);
    }
}

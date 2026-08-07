#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "../../engine/analysis/statistics/accumulator.hpp"

using Catch::Approx;
using aud::Sample;
using aud::statistics::ChannelAccumulator;

namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

TEST_CASE("ChannelAccumulator: DC signal has zero variance and dcOffset equal to the level", "[statistics]") {
    ChannelAccumulator acc;
    acc.begin(48000);
    std::vector<Sample> dc(48000, 0.25f);
    acc.process(dc, 0);
    acc.finish();

    CHECK(acc.mean() == Approx(0.25).margin(1e-9));
    CHECK(acc.variance() == Approx(0.0).margin(1e-12));
    CHECK(acc.stdDev() == Approx(0.0).margin(1e-9));
    CHECK(acc.peak() == Approx(0.25));
    CHECK(acc.minValue() == Approx(0.25));
    CHECK(acc.maxValue() == Approx(0.25));
    CHECK(acc.zeroCrossings() == 0);
}

TEST_CASE("ChannelAccumulator: full-scale sine has peak 1.0 and RMS 0.7071", "[statistics]") {
    constexpr aud::SampleRate sampleRate = 48000;
    constexpr std::size_t     period     = 480;  // exact whole periods across the buffer
    constexpr std::size_t     n          = period * 200;

    std::vector<Sample> sine(n);
    for (std::size_t i = 0; i < n; ++i) {
        sine[i] = static_cast<Sample>(std::sin(2.0 * kPi * static_cast<double>(i) / static_cast<double>(period)));
    }

    ChannelAccumulator acc;
    acc.begin(sampleRate);
    acc.process(sine, 0);
    acc.finish();

    CHECK(acc.peak() == Approx(1.0).margin(1e-6));
    CHECK(acc.rms() == Approx(0.70710678118).margin(1e-4));
    CHECK(acc.mean() == Approx(0.0).margin(1e-6));

    // Crest factor derived the same way StatisticsAnalyzer does.
    const double crestDb = 20.0 * std::log10(acc.peak() / acc.rms());
    CHECK(crestDb == Approx(3.0103).margin(0.01));
}

TEST_CASE("ChannelAccumulator: full-scale square wave has crest factor ~0 dB", "[statistics]") {
    constexpr aud::SampleRate sampleRate = 48000;
    constexpr std::size_t     halfPeriod = 20;
    constexpr std::size_t     n          = halfPeriod * 2 * 500;

    std::vector<Sample> square(n);
    for (std::size_t i = 0; i < n; ++i) {
        square[i] = ((i / halfPeriod) % 2 == 0) ? 1.0f : -1.0f;
    }

    ChannelAccumulator acc;
    acc.begin(sampleRate);
    acc.process(square, 0);
    acc.finish();

    CHECK(acc.peak() == Approx(1.0).margin(1e-9));
    CHECK(acc.rms() == Approx(1.0).margin(1e-9));
    const double crestDb = 20.0 * std::log10(acc.peak() / acc.rms());
    CHECK(crestDb == Approx(0.0).margin(1e-6));

    // Exactly one crossing per half-period boundary (minus the very first, which has no
    // predecessor) — n/halfPeriod - 1 crossings.
    CHECK(acc.zeroCrossings() == n / halfPeriod - 1);
}

TEST_CASE("ChannelAccumulator: peak frame index points at the loudest sample", "[statistics]") {
    std::vector<Sample> samples(1000, 0.1f);
    samples[437] = -0.9f;
    samples[600] = 0.5f;

    ChannelAccumulator acc;
    acc.begin(44100);
    acc.process(samples, 0);
    acc.finish();

    CHECK(acc.peak() == Approx(0.9));
    CHECK(acc.peakFrame() == 437);
}

TEST_CASE("ChannelAccumulator: peak frame index accounts for chunk startFrame offset", "[statistics]") {
    ChannelAccumulator acc;
    acc.begin(44100);

    std::vector<Sample> chunk0(500, 0.1f);
    std::vector<Sample> chunk1(500, 0.1f);
    chunk1[42] = 0.8f;

    acc.process(chunk0, 0);
    acc.process(chunk1, 500);
    acc.finish();

    CHECK(acc.peakFrame() == 542);
}

TEST_CASE("ChannelAccumulator: windowed RMS series has one entry per 50ms window", "[statistics]") {
    constexpr aud::SampleRate sampleRate = 48000;
    const std::size_t         windowFrames = sampleRate / 20;

    std::vector<Sample> samples(windowFrames * 4, 0.5f);

    ChannelAccumulator acc;
    acc.begin(sampleRate);
    acc.process(samples, 0);
    acc.finish();

    REQUIRE(acc.rmsSeries().size() == 4);
    for (float rms : acc.rmsSeries()) {
        CHECK(rms == Approx(0.5).margin(1e-6));
    }
}

TEST_CASE("ChannelAccumulator: silence produces zero peak, rms and no crossings", "[statistics]") {
    std::vector<Sample> silence(10000, 0.0f);

    ChannelAccumulator acc;
    acc.begin(44100);
    acc.process(silence, 0);
    acc.finish();

    CHECK(acc.peak() == 0.0);
    CHECK(acc.rms() == 0.0);
    CHECK(acc.zeroCrossings() == 0);
    CHECK(acc.peakFrame() == aud::kNoFrame);
}

TEST_CASE("ChannelAccumulator: all-zero windows are flagged for M10 digital silence", "[statistics]") {
    constexpr aud::SampleRate sampleRate    = 48000;
    constexpr std::size_t     windowFrames  = 2400;  // 50ms @ 48kHz

    std::vector<Sample> samples(windowFrames * 3, 0.0f);
    samples[windowFrames + 5] = 0.01f;  // one nonzero sample in the middle window only

    ChannelAccumulator acc;
    acc.begin(sampleRate);
    acc.process(samples, 0);
    acc.finish();

    REQUIRE(acc.allZeroSeries().size() == 3);
    CHECK(acc.allZeroSeries()[0] == 1);
    CHECK(acc.allZeroSeries()[1] == 0);
    CHECK(acc.allZeroSeries()[2] == 1);
}

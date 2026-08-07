#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <span>
#include <vector>

#include "../../engine/analysis/statistics/statistics_analyzer.hpp"

using Catch::Approx;
using aud::AudioSpec;
using aud::ChunkView;
using aud::Sample;
using aud::statistics::makeStatisticsAnalyzer;
using aud::statistics::StatisticsConfig;
using aud::statistics::StatisticsResult;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Feeds one mono or stereo signal through StatisticsAnalyzer in a single chunk and returns the
// finished result.
StatisticsResult runMono(const std::vector<Sample>& mono, aud::SampleRate sampleRate = 48000,
                          std::uint32_t containerBitDepth = 0) {
    StatisticsResult result;
    StatisticsConfig  config;
    config.containerBitDepth = containerBitDepth;
    auto analyzer = makeStatisticsAnalyzer(result, config);

    REQUIRE(analyzer->begin(AudioSpec{sampleRate, 1, static_cast<aud::FrameIndex>(mono.size())}).has_value());

    std::vector<std::span<const Sample>> channels{mono};
    ChunkView                            view{channels, 0};
    REQUIRE(analyzer->process(view).has_value());
    REQUIRE(analyzer->finish().has_value());
    return result;
}

StatisticsResult runStereo(const std::vector<Sample>& left, const std::vector<Sample>& right,
                            aud::SampleRate sampleRate = 48000) {
    StatisticsResult result;
    auto              analyzer = makeStatisticsAnalyzer(result);

    REQUIRE(analyzer->begin(AudioSpec{sampleRate, 2, static_cast<aud::FrameIndex>(left.size())}).has_value());

    std::vector<std::span<const Sample>> channels{left, right};
    ChunkView                            view{channels, 0};
    REQUIRE(analyzer->process(view).has_value());
    REQUIRE(analyzer->finish().has_value());
    return result;
}
}  // namespace

TEST_CASE("statistics: full-scale sine reports peak 1.0, RMS 0.7071, crest 3.01 dB", "[statistics]") {
    constexpr std::size_t period = 480;
    std::vector<Sample>   sine(period * 200);
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = static_cast<Sample>(std::sin(2.0 * kPi * static_cast<double>(i) / static_cast<double>(period)));
    }

    auto result = runMono(sine);
    REQUIRE(result.channels.size() == 1);
    const auto& c = result.channels[0];

    CHECK(c.peak == Approx(1.0).margin(1e-6));
    CHECK(c.rms == Approx(0.70710678118).margin(1e-4));
    CHECK(c.crestFactorDb == Approx(3.0103).margin(0.01));
    CHECK(result.crestFactorDb == Approx(3.0103).margin(0.01));
}

TEST_CASE("statistics: full-scale square wave reports crest factor 0 dB", "[statistics]") {
    constexpr std::size_t halfPeriod = 20;
    std::vector<Sample>   square(halfPeriod * 2 * 500);
    for (std::size_t i = 0; i < square.size(); ++i) {
        square[i] = ((i / halfPeriod) % 2 == 0) ? 1.0f : -1.0f;
    }

    auto result = runMono(square);
    const auto& c = result.channels[0];
    CHECK(c.crestFactorDb == Approx(0.0).margin(1e-6));
}

TEST_CASE("statistics: white noise has crest factor roughly 11-13 dB", "[statistics]") {
    // "White noise" in the ~11-13 dB crest-factor sense (M09's acceptance criteria) means Gaussian
    // noise, not bounded uniform noise: uniform[-1,1] has a fixed, deterministic crest factor of
    // 20*log10(sqrt(3)) ~ 4.77 dB (peak/RMS is bounded by construction), which isn't the case being
    // tested here. Gaussian noise's peak grows with the sample count via extreme-value statistics
    // (~sqrt(2*ln(n)) standard deviations for n samples), landing in the low-teens of dB for
    // hundreds of thousands of samples.
    std::mt19937                    rng(5);
    std::normal_distribution<float> dist(0.0f, 0.1f);
    std::vector<Sample>              noise(500000);
    for (auto& s : noise) s = dist(rng);

    auto result = runMono(noise);
    const auto& c = result.channels[0];
    CHECK(c.crestFactorDb > 9.0);
    CHECK(c.crestFactorDb < 17.0);
}

TEST_CASE("statistics: white noise between two independent channels correlates close to 0", "[statistics]") {
    std::mt19937                          rngL(11), rngR(22);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    constexpr std::size_t                  n = 300000;
    std::vector<Sample>                    left(n), right(n);
    for (auto& s : left) s = dist(rngL);
    for (auto& s : right) s = dist(rngR);

    auto result = runStereo(left, right);
    REQUIRE(result.stereo.has_value());
    CHECK(std::fabs(result.stereo->correlation) < 0.02);
}

TEST_CASE("statistics: identical channels correlate exactly 1.0", "[statistics]") {
    std::mt19937                          rng(6);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<Sample>                    signal(20000);
    for (auto& s : signal) s = dist(rng);

    auto result = runStereo(signal, signal);
    REQUIRE(result.stereo.has_value());
    CHECK(result.stereo->correlation == Approx(1.0).margin(1e-9));
}

TEST_CASE("statistics: inverted channels correlate exactly -1.0 and mono compatibility is -inf", "[statistics]") {
    std::mt19937                          rng(9);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<Sample>                    left(20000);
    for (auto& s : left) s = dist(rng);
    std::vector<Sample> right(left.size());
    for (std::size_t i = 0; i < left.size(); ++i) right[i] = -left[i];

    auto result = runStereo(left, right);
    REQUIRE(result.stereo.has_value());
    CHECK(result.stereo->correlation == Approx(-1.0).margin(1e-9));
    CHECK(std::isinf(result.stereo->monoCompatibilityDb));
    CHECK(result.stereo->monoCompatibilityDb < 0.0);
}

TEST_CASE("statistics: DC signal has zero variance and dcOffset equal to the level", "[statistics]") {
    std::vector<Sample> dc(48000, -0.3f);
    auto                 result = runMono(dc);
    const auto&           c      = result.channels[0];
    CHECK(c.dcOffset == Approx(-0.3).margin(1e-9));
    CHECK(c.variance == Approx(0.0).margin(1e-12));
}

TEST_CASE("statistics: mono channel produces no stereo statistics", "[statistics]") {
    std::vector<Sample> mono(1000, 0.1f);
    auto                 result = runMono(mono);
    CHECK_FALSE(result.stereo.has_value());
}

TEST_CASE("statistics: histogram sums to the sample count", "[statistics]") {
    std::mt19937                          rng(13);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<Sample>                    signal(10000);
    for (auto& s : signal) s = dist(rng);

    auto result = runMono(signal);
    std::uint64_t total = 0;
    for (auto count : result.channels[0].histogram) total += count;
    CHECK(total == signal.size());
}

TEST_CASE("statistics: interleaved RMS series has channelCount entries per window", "[statistics]") {
    std::vector<Sample> left(48000 * 2, 0.5f);
    std::vector<Sample> right(48000 * 2, 0.25f);

    auto result = runStereo(left, right);
    REQUIRE(result.rmsSeriesChannelCount == 2);
    REQUIRE(result.rmsSeries.size() % 2 == 0);
    // Every even-indexed entry is channel 0 (~0.5), every odd-indexed is channel 1 (~0.25).
    for (std::size_t i = 0; i < result.rmsSeries.size(); i += 2) {
        CHECK(result.rmsSeries[i] == Approx(0.5).margin(1e-4));
        CHECK(result.rmsSeries[i + 1] == Approx(0.25).margin(1e-4));
    }
}

TEST_CASE("statistics: JSON report round-trips the schema's top-level shape", "[statistics]") {
    std::vector<Sample> mono(2000, 0.1f);
    auto                 result = runMono(mono);
    const std::string     json   = result.toJson();

    CHECK(json.find("\"schemaVersion\":\"1.0.0\"") != std::string::npos);
    CHECK(json.find("\"channels\":[") != std::string::npos);
    CHECK(json.find("\"histogram\":[") != std::string::npos);
    CHECK(json.find("\"stereo\":null") != std::string::npos);
    CHECK(json.find("\"rmsSeries\":[") != std::string::npos);
}

TEST_CASE("statistics: 16-bit content in a 24-bit container is reported per channel", "[statistics]") {
    constexpr double    scale = 1 << 23;  // 24-bit
    std::mt19937          rng(21);
    std::uniform_int_distribution<int> dist(-32768, 32767);
    std::vector<Sample>   samples(20000);
    for (auto& s : samples) {
        // Exact multiples of 2^8 in the 24-bit domain == genuine 16-bit content.
        const int v16 = dist(rng);
        s              = static_cast<Sample>(static_cast<double>(v16) * 256.0 / scale);
    }

    auto result = runMono(samples, 48000, 24);
    const auto& c = result.channels[0];
    REQUIRE(c.bitDepth.effectiveBitDepth.has_value());
    CHECK(*c.bitDepth.effectiveBitDepth == 16);
}

TEST_CASE("statistics: multiple process() calls accumulate the same as one big call", "[statistics]") {
    std::mt19937                          rng(31);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<Sample>                    signal(20000);
    for (auto& s : signal) s = dist(rng);

    auto whole = runMono(signal);

    StatisticsResult chunked;
    auto              analyzer = makeStatisticsAnalyzer(chunked);
    REQUIRE(analyzer->begin(AudioSpec{48000, 1, static_cast<aud::FrameIndex>(signal.size())}).has_value());

    constexpr std::size_t chunkSize = 3333;
    for (std::size_t offset = 0; offset < signal.size(); offset += chunkSize) {
        const std::size_t                     len = std::min(chunkSize, signal.size() - offset);
        std::vector<std::span<const Sample>>  channels{std::span<const Sample>(signal).subspan(offset, len)};
        ChunkView                              view{channels, static_cast<aud::FrameIndex>(offset)};
        REQUIRE(analyzer->process(view).has_value());
    }
    REQUIRE(analyzer->finish().has_value());

    CHECK(chunked.channels[0].peak == Approx(whole.channels[0].peak).margin(1e-9));
    CHECK(chunked.channels[0].rms == Approx(whole.channels[0].rms).margin(1e-9));
    CHECK(chunked.channels[0].dcOffset == Approx(whole.channels[0].dcOffset).margin(1e-9));
    CHECK(chunked.channels[0].peakFrame == whole.channels[0].peakFrame);
}

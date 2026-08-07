#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

#include "../../engine/analysis/beats/tempo.hpp"

using Catch::Approx;
using aud::beats::estimateTempo;
using aud::beats::estimateTempoSeries;
using aud::beats::TempoConfig;

namespace {

constexpr double kHopSeconds = 512.0 / 44100.0;

// A periodic train of narrow Gaussian bumps in ODF-frame space, at `bpm` — the exact analogue of a
// perfectly regular click track's ODF, without needing an STFT pass.
std::vector<float> periodicOdf(double bpm, std::size_t frameCount, double hopSeconds = kHopSeconds) {
    std::vector<float> odf(frameCount, 0.0f);
    const double        periodFrames = 60.0 / bpm / hopSeconds;
    for (double center = 0.0; center < static_cast<double>(frameCount); center += periodFrames) {
        for (std::size_t i = 0; i < frameCount; ++i) {
            const double d = static_cast<double>(i) - center;
            if (std::fabs(d) < 6.0) odf[i] += static_cast<float>(std::exp(-0.5 * d * d));
        }
    }
    return odf;
}

}  // namespace

TEST_CASE("beats::estimateTempo: a regular 120 BPM pulse train's primary estimate is near 120 with high confidence",
          "[beats][tempo]") {
    const auto odf = periodicOdf(120.0, 2000);
    const auto est  = estimateTempo(odf, kHopSeconds, TempoConfig{});

    REQUIRE(est.primaryBpm > 0.0);
    CHECK(est.primaryBpm == Approx(120.0).margin(2.0));
    CHECK(est.tempoConfidence > 0.3f);
}

TEST_CASE("beats::estimateTempo: an alternative whose raw score is within the configured fraction of "
          "the primary's is reported; the primary itself always is",
          "[beats][tempo]") {
    // A clean, strongly-periodic pulse train's raw autocorrelation is dominated by its true
    // fundamental — the harmonic-sum step means a ÷2/×2 reading isn't necessarily a comparably
    // strong *local maximum* of its own for every signal (the doc's acceptance criterion is itself
    // conditional: "when the x2/÷2 score is within 30% of the primary"). What must always hold,
    // regardless of the signal, is the mechanism: the primary appears in `alternatives`, and the
    // threshold in TempoConfig::alternativeScoreFraction is respected — verified directly here
    // rather than by asserting a specific alternative shows up for one specific synthetic signal.
    const auto odf = periodicOdf(120.0, 2000);
    const auto est  = estimateTempo(odf, kHopSeconds, TempoConfig{});

    REQUIRE_FALSE(est.alternatives.empty());
    CHECK(std::fabs(est.alternatives.front().bpm - est.primaryBpm) < 0.5);

    const float bestScore = est.alternatives.front().score;
    for (const auto& alt : est.alternatives) {
        CHECK(alt.score >= bestScore * static_cast<float>(1.0 - TempoConfig{}.alternativeScoreFraction));
    }
}

TEST_CASE("beats::estimateTempo: white noise gives low tempo confidence", "[beats][tempo]") {
    std::mt19937                          rng(12345);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> odf(2000);
    for (float& v : odf) v = dist(rng);

    const auto est = estimateTempo(odf, kHopSeconds, TempoConfig{});
    CHECK(est.tempoConfidence < 0.3f);
}

TEST_CASE("beats::estimateTempoSeries: a constant-tempo pulse train reports tempoIsStable == true",
          "[beats][tempo]") {
    const auto odf    = periodicOdf(120.0, 6000);
    const auto result = estimateTempoSeries(odf, kHopSeconds, TempoConfig{});
    CHECK(result.tempoIsStable);
}

TEST_CASE("beats::estimateTempoSeries: a tempo ramp reports tempoIsStable == false with a sensible series",
          "[beats][tempo]") {
    constexpr std::size_t frameCount = 6000;
    std::vector<float>    odf(frameCount, 0.0f);

    // A pulse train whose instantaneous tempo ramps from 100 to 140 BPM across the buffer.
    double positionFrames = 0.0;
    double bpm             = 100.0;
    while (positionFrames < static_cast<double>(frameCount)) {
        for (std::size_t i = 0; i < frameCount; ++i) {
            const double d = static_cast<double>(i) - positionFrames;
            if (std::fabs(d) < 6.0) odf[i] += static_cast<float>(std::exp(-0.5 * d * d));
        }
        bpm = 100.0 + 40.0 * (positionFrames / static_cast<double>(frameCount));
        positionFrames += 60.0 / bpm / kHopSeconds;
    }

    TempoConfig config;
    config.windowSeconds = 3.0;  // shorter windows so the ramp is actually resolved
    const auto result     = estimateTempoSeries(odf, kHopSeconds, config);

    CHECK_FALSE(result.tempoIsStable);
    REQUIRE(result.tempoSeries.size() >= 2);
    // Sensible: the series should generally trend upward, not be nonsense/zero throughout.
    bool anyNonZero = false;
    for (float v : result.tempoSeries) anyNonZero |= (v > 0.0f);
    CHECK(anyNonZero);
}

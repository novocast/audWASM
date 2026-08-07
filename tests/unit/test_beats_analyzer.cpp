#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <span>
#include <vector>

#include "../../engine/analysis/beats/beat_analyzer.hpp"

using Catch::Approx;
using aud::AudioSpec;
using aud::ChunkView;
using aud::Sample;
using aud::beats::BeatConfig;
using aud::beats::BeatResult;
using aud::beats::makeBeatAnalyzer;

namespace {

constexpr aud::SampleRate kSampleRate = 44100;

// A click track: a single-sample impulse at every beat, starting at `firstBeatSeconds`, spaced by
// `periodSeconds`, for `durationSeconds` — the ideal broadband transient (same idea as
// test_stft.cpp's centring test), so onset/tempo/beat detection has no ambiguity about "where's the
// transient" and the test isolates this module's own timing behaviour.
std::vector<Sample> clickTrack(double periodSeconds, double firstBeatSeconds, double durationSeconds,
                                 std::vector<double>* beatTimesOut = nullptr) {
    std::vector<Sample> samples(static_cast<std::size_t>(durationSeconds * kSampleRate), 0.0f);
    for (double t = firstBeatSeconds; t < durationSeconds; t += periodSeconds) {
        const auto index = static_cast<std::size_t>(std::llround(t * kSampleRate));
        if (index < samples.size()) samples[index] = 1.0f;
        if (beatTimesOut) beatTimesOut->push_back(t);
    }
    return samples;
}

BeatResult run(const std::vector<Sample>& mono, BeatConfig config = {}) {
    BeatResult result;
    auto        analyzer = makeBeatAnalyzer(result, config);

    REQUIRE(analyzer->begin(AudioSpec{kSampleRate, 1, static_cast<aud::FrameIndex>(mono.size())}).has_value());

    std::vector<std::span<const Sample>> channels{mono};
    ChunkView                            view{channels, 0};
    REQUIRE(analyzer->process(view).has_value());
    REQUIRE(analyzer->finish().has_value());
    return result;
}

}  // namespace

TEST_CASE("beats: a click track at exactly 120 BPM reports 120.0 +-0.1 BPM, beats within +-5ms",
          "[beats][analyzer]") {
    std::vector<double> trueBeats;
    const auto           samples = clickTrack(0.5, 0.0, 10.0, &trueBeats);

    const auto result = run(samples);

    REQUIRE(result.primaryBpm > 0.0);
    CHECK(result.primaryBpm == Approx(120.0).margin(0.1));

    REQUIRE(result.beats.size() > 10);
    for (double trueBeat : trueBeats) {
        double bestDelta = 1e9;
        for (const auto& beat : result.beats) bestDelta = std::min(bestDelta, std::fabs(beat.timeSeconds - trueBeat));
        CHECK(bestDelta < 0.005);
    }
}

TEST_CASE("beats: a 120 BPM click track with the first beat at t=0.25s is tracked at the correct phase",
          "[beats][analyzer]") {
    std::vector<double> trueBeats;
    const auto           samples = clickTrack(0.5, 0.25, 10.0, &trueBeats);

    const auto result = run(samples);

    REQUIRE_FALSE(result.beats.empty());
    // At least one detected beat should sit near the true first beat at t=0.25s. This is the very
    // first transient in the whole track — the ODF has no history before it, which measurably
    // (~10ms, not the ~1-2ms steady-state accuracy seen once a track is under way) shifts its own
    // median/MAD normalisation window; a one-off edge effect at the start of a file, not the
    // systematic bias the "onset timing is not systematically biased" test below guards against.
    double bestDelta = 1e9;
    for (const auto& beat : result.beats) bestDelta = std::min(bestDelta, std::fabs(beat.timeSeconds - 0.25));
    CHECK(bestDelta < 0.015);
}

TEST_CASE("beats: white noise reports low tempo confidence rather than a confidently-wrong BPM",
          "[beats][analyzer]") {
    std::mt19937                          rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<Sample> samples(kSampleRate * 5);
    for (Sample& s : samples) s = dist(rng);

    const auto result = run(samples);
    // Measured ~0.35 for this seed — comfortably below a confident reading (>0.7-0.8, where a real
    // click track lands) but not the near-zero a perfectly flat autocorrelation would give, since
    // finite-length white noise still has some spurious structure. The UI-facing acceptance
    // criterion (M13 doc) is comparative — low F-measure must pair with low *reported* confidence,
    // not an absolute number — this just guards against a regression toward "confident."
    CHECK(result.tempoConfidence < 0.4f);
}

TEST_CASE("beats: a tempo ramp from 100 to 140 BPM reports tempoIsStable == false with a sensible series",
          "[beats][analyzer]") {
    std::vector<Sample> samples(static_cast<std::size_t>(20.0 * kSampleRate), 0.0f);

    double t   = 0.0;
    double bpm = 100.0;
    while (t < 20.0) {
        const auto index = static_cast<std::size_t>(std::llround(t * kSampleRate));
        if (index < samples.size()) samples[index] = 1.0f;
        bpm = 100.0 + 40.0 * (t / 20.0);
        t += 60.0 / bpm;
    }

    BeatConfig config;
    config.tempo.windowSeconds = 4.0;
    const auto result           = run(samples, config);

    CHECK_FALSE(result.tempoIsStable);
    REQUIRE(result.tempoSeries.size() >= 2);
}

TEST_CASE("beats: onset timing is not systematically biased (the M06 centring guard, at this level)",
          "[beats][analyzer]") {
    std::vector<double> trueTimes;
    const auto           samples = clickTrack(0.37, 0.1, 15.0, &trueTimes);  // an irregular-ish period, deliberately

    const auto result = run(samples);
    REQUIRE(result.onsets.size() >= trueTimes.size() / 2);

    double signedErrorSum = 0.0;
    int    matched          = 0;
    for (double trueTime : trueTimes) {
        double bestDelta = 1e9, bestSigned = 0.0;
        for (const auto& onset : result.onsets) {
            const double delta = std::fabs(onset.timeSeconds - trueTime);
            if (delta < bestDelta) {
                bestDelta  = delta;
                bestSigned = onset.timeSeconds - trueTime;
            }
        }
        if (bestDelta < 0.01) {
            signedErrorSum += bestSigned;
            ++matched;
        }
    }

    REQUIRE(matched > 5);
    const double meanSignedError = signedErrorSum / matched;
    CHECK(std::fabs(meanSignedError) < 0.002);  // no systematic (one-sided) bias, just jitter
}

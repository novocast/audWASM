#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <span>
#include <vector>

#include "../../engine/analysis/transients/click_detector.hpp"
#include "../../engine/analysis/transients/dropout_detector.hpp"
#include "../../engine/analysis/transients/refine_timing.hpp"
#include "../../engine/analysis/transients/transient_analyzer.hpp"

using Catch::Approx;
using aud::AudioSpec;
using aud::ChunkView;
using aud::Sample;
using aud::transients::ClickDetectorConfig;
using aud::transients::DropoutDetectorConfig;
using aud::transients::makeTransientAnalyzer;
using aud::transients::RefineTimingConfig;
using aud::transients::TransientCandidate;
using aud::transients::TransientClass;
using aud::transients::TransientConfig;
using aud::transients::TransientResult;

namespace {

constexpr aud::SampleRate kSampleRate = 44100;

// A decaying tone at `freqHz`, amplitude `amplitude` at t=0, decaying with time-constant `tauSeconds`
// (amplitude(t) = amplitude * exp(-t/tau)), starting at `startFrame` inside a `totalFrames`-long
// otherwise-silent buffer. This is the same "isolated, unambiguous transient" idea
// test_beats_analyzer.cpp's clickTrack() uses, adapted to give the classifier real spectral/decay
// character instead of a single-sample impulse.
std::vector<Sample> decayingTone(double freqHz, double tauSeconds, std::size_t startFrame, std::size_t totalFrames,
                                   double amplitude = 0.8) {
    std::vector<Sample> samples(totalFrames, 0.0f);
    for (std::size_t i = startFrame; i < totalFrames; ++i) {
        const double t = static_cast<double>(i - startFrame) / kSampleRate;
        samples[i]      = static_cast<float>(amplitude * std::exp(-t / tauSeconds) * std::sin(2.0 * M_PI * freqHz * t));
    }
    return samples;
}

// Broadband noise burst decaying the same way, optionally with a low-mid resonant tone mixed in
// (doc's snare rule: "broadband ... plus a body"). rng is seeded by the caller for reproducibility.
std::vector<Sample> decayingNoise(std::mt19937& rng, double tauSeconds, std::size_t startFrame,
                                    std::size_t totalFrames, double amplitude = 0.5, double bodyFreqHz = 0.0,
                                    double bodyAmplitude = 0.0) {
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<Sample>                    samples(totalFrames, 0.0f);
    for (std::size_t i = startFrame; i < totalFrames; ++i) {
        const double t       = static_cast<double>(i - startFrame) / kSampleRate;
        const double envelope = std::exp(-t / tauSeconds);
        double        value    = amplitude * envelope * dist(rng);
        if (bodyAmplitude > 0.0) value += bodyAmplitude * envelope * std::sin(2.0 * M_PI * bodyFreqHz * t);
        samples[i] = static_cast<float>(value);
    }
    return samples;
}

TransientResult analyze(const std::vector<Sample>& mono, std::vector<TransientCandidate> candidates,
                         TransientConfig config = {}) {
    TransientResult result;
    auto             analyzer = makeTransientAnalyzer(result, std::move(candidates), config);

    REQUIRE(analyzer->begin(AudioSpec{kSampleRate, 1, static_cast<aud::FrameIndex>(mono.size())}).has_value());
    std::vector<std::span<const Sample>> channels{mono};
    ChunkView                            view{channels, 0};
    REQUIRE(analyzer->process(view).has_value());
    REQUIRE(analyzer->finish().has_value());
    return result;
}

}  // namespace

// --- Timing accuracy ---

TEST_CASE("transients: refineTransientTiming locates an isolated impulse within +-1ms",
          "[transients][timing]") {
    constexpr std::size_t kTrueFrame = 22050;  // t = 0.5s
    std::vector<Sample>   samples(kSampleRate * 2, 0.0f);
    samples[kTrueFrame] = 1.0f;

    const auto refined = aud::transients::refineTransientTiming(samples, kSampleRate, static_cast<aud::FrameIndex>(kTrueFrame));

    REQUIRE(refined.attackFrame != aud::kNoFrame);
    const double deltaMs = std::fabs(static_cast<double>(refined.attackFrame) - static_cast<double>(kTrueFrame)) *
                            1000.0 / kSampleRate;
    CHECK(deltaMs < 1.0);
}

TEST_CASE("transients: refineTransientTiming's startFrame precedes attackFrame at a zero crossing",
          "[transients][timing]") {
    // A decaying tone starting exactly at zero and rising — the last zero crossing before the
    // steepest rise should sit at (or just before) the tone's true start.
    constexpr std::size_t kStartFrame = 10000;
    const auto             samples     = decayingTone(200.0, 0.05, kStartFrame, kSampleRate);

    const auto refined =
        aud::transients::refineTransientTiming(samples, kSampleRate, static_cast<aud::FrameIndex>(kStartFrame + 50));

    REQUIRE(refined.startFrame != aud::kNoFrame);
    REQUIRE(refined.attackFrame != aud::kNoFrame);
    CHECK(refined.startFrame <= refined.attackFrame);
    // Within the +-20ms search window of the true onset — not tighter, since "last zero crossing"
    // on a 200Hz tone can land on any of the tone's own zero crossings near the start, and the
    // steepest-rise point itself is only guaranteed to fall inside the search window.
    const double deltaMs =
        std::fabs(static_cast<double>(refined.startFrame) - static_cast<double>(kStartFrame)) * 1000.0 / kSampleRate;
    CHECK(deltaMs < 20.0);
}

// --- Classification ---

TEST_CASE("transients: an isolated synthesised kick is classified Kick", "[transients][classifier]") {
    constexpr std::size_t kStart = 5000;
    const auto             mono   = decayingTone(55.0, 0.05, kStart, kSampleRate);

    const auto result =
        analyze(mono, {TransientCandidate{static_cast<double>(kStart) / kSampleRate, 1.0f}});

    REQUIRE(result.transients.size() == 1);
    CHECK(result.transients[0].classification == TransientClass::Kick);
    CHECK(result.transients[0].classConfidence > 0.5f);
}

TEST_CASE("transients: an isolated synthesised snare is classified Snare", "[transients][classifier]") {
    std::mt19937            rng(1);
    constexpr std::size_t kStart = 5000;
    const auto              mono = decayingNoise(rng, 0.08, kStart, kSampleRate, /*amplitude=*/0.6,
                                                    /*bodyFreqHz=*/200.0, /*bodyAmplitude=*/0.15);

    const auto result =
        analyze(mono, {TransientCandidate{static_cast<double>(kStart) / kSampleRate, 1.0f}});

    REQUIRE(result.transients.size() == 1);
    CHECK(result.transients[0].classification == TransientClass::Snare);
}

TEST_CASE("transients: an isolated synthesised hi-hat is classified HiHat", "[transients][classifier]") {
    std::mt19937            rng(2);
    constexpr std::size_t kStart = 5000;
    const auto              mono = decayingNoise(rng, 0.03, kStart, kSampleRate, /*amplitude=*/0.5);

    const auto result =
        analyze(mono, {TransientCandidate{static_cast<double>(kStart) / kSampleRate, 1.0f}});

    REQUIRE(result.transients.size() == 1);
    CHECK(result.transients[0].classification == TransientClass::HiHat);
}

// --- Defect detection ---

TEST_CASE("transients: a deliberately inserted single-sample click is detected", "[transients][click]") {
    std::mt19937                           rng(3);
    std::uniform_real_distribution<double> noiseDist(-0.02, 0.02);

    std::vector<Sample> mono(kSampleRate, 0.0f);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        mono[i]         = static_cast<float>(0.3 * std::sin(2.0 * M_PI * 440.0 * t) + noiseDist(rng));
    }
    constexpr std::size_t kClickFrame = 20000;
    mono[kClickFrame] += 0.5f;  // a single-sample discontinuity well above the LPC residual floor

    auto candidates = aud::transients::detectClicks(mono, kSampleRate);
    REQUIRE_FALSE(candidates.empty());

    bool found = false;
    for (const auto& c : candidates) {
        if (std::llabs(static_cast<long long>(c.frame) - static_cast<long long>(kClickFrame)) <= 2) found = true;
    }
    CHECK(found);
}

TEST_CASE("transients: a snare hit is not reported as a click once its onset is known",
          "[transients][click]") {
    std::mt19937            rng(4);
    constexpr std::size_t kStart = 5000;
    const auto              mono = decayingNoise(rng, 0.08, kStart, kSampleRate, /*amplitude=*/0.6,
                                                    /*bodyFreqHz=*/200.0, /*bodyAmplitude=*/0.15);

    auto candidates = aud::transients::detectClicks(mono, kSampleRate);
    const std::vector<double> onsetTimes{static_cast<double>(kStart) / kSampleRate};
    candidates = aud::transients::rejectOnsetCoincidences(std::move(candidates), kSampleRate, onsetTimes, 5.0);

    for (const auto& c : candidates) {
        const double deltaMs = std::fabs(static_cast<double>(c.frame) - static_cast<double>(kStart)) * 1000.0 / kSampleRate;
        CHECK(deltaMs > 5.0);
    }
}

TEST_CASE("transients: a 5ms dropout inserted into music is detected as a Dropout", "[transients][dropout]") {
    std::vector<Sample> mono(kSampleRate, 0.0f);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        mono[i]         = static_cast<float>(0.4 * std::sin(2.0 * M_PI * 300.0 * t));
    }
    constexpr std::size_t kDropoutStart  = 30000;
    constexpr std::size_t kDropoutLength = static_cast<std::size_t>(0.005 * kSampleRate);  // 5ms
    for (std::size_t i = kDropoutStart; i < kDropoutStart + kDropoutLength; ++i) mono[i] = 0.0f;

    const auto runs = aud::transients::detectDropouts(mono, kSampleRate);
    REQUIRE_FALSE(runs.empty());

    bool found = false;
    for (const auto& run : runs) {
        if (run.begin <= static_cast<aud::FrameIndex>(kDropoutStart) &&
            run.end >= static_cast<aud::FrameIndex>(kDropoutStart + kDropoutLength)) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("transients: clean material produces zero defects", "[transients][clean]") {
    std::vector<Sample> mono(kSampleRate, 0.0f);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        mono[i]         = static_cast<float>(0.4 * std::sin(2.0 * M_PI * 300.0 * t));
    }

    const auto result = analyze(mono, {});
    CHECK(result.defects.empty());
}

// --- Benchmark: transient pass cost on top of the (already-computed, cost not included here) STFT. ---

TEST_CASE("transients: analysis pass benchmark", "[.][transients][benchmark]") {
    std::mt19937            rng(5);
    std::vector<Sample>    mono(kSampleRate * 30, 0.0f);  // 30s
    std::vector<TransientCandidate> candidates;
    for (double t = 0.0; t < 30.0; t += 0.5) {
        const auto start = static_cast<std::size_t>(t * kSampleRate);
        const auto tone   = decayingTone(80.0 + 40.0 * (candidates.size() % 3), 0.08, start, mono.size());
        for (std::size_t i = 0; i < mono.size(); ++i) mono[i] += tone[i];
        candidates.push_back(TransientCandidate{t, 1.0f});
    }

    BENCHMARK("60 candidates over 30s mono @ 44.1kHz") {
        return analyze(mono, candidates);
    };
}

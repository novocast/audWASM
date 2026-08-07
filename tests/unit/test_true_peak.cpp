#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "../../engine/analysis/loudness/true_peak.hpp"

using Catch::Approx;
using aud::Sample;
using aud::loudness::TruePeakMeter;

namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

TEST_CASE("true_peak: DC signal has matching sample peak and true peak", "[loudness]") {
    constexpr aud::SampleRate sampleRate = 48000;
    TruePeakMeter             meter(sampleRate, 1, 4);

    // Ramp in and back out gently around the sustained 0.5 plateau: an instantaneous step (to or
    // from silence) is a genuine discontinuity, and any sinc-based reconstruction filter (this one
    // included) rings above the target level for a few samples either side of a hard edge (Gibbs
    // phenomenon) — real, and not specific to this implementation (finish()/drain() flushes the
    // filter's tail by treating end-of-stream as "followed by silence", which is itself a hard
    // edge whenever the file simply stops on a non-zero sample), but not what this test is trying
    // to isolate. The sustained plateau in between is what should read flat at the DC level for
    // both peak measures.
    std::vector<Sample> samples(2400);
    for (std::size_t i = 0; i < 200; ++i) {
        samples[i] = static_cast<Sample>(0.5 * static_cast<double>(i) / 200.0);
    }
    for (std::size_t i = 200; i < 2200; ++i) samples[i] = 0.5f;
    for (std::size_t i = 2200; i < samples.size(); ++i) {
        samples[i] = static_cast<Sample>(0.5 * (1.0 - static_cast<double>(i - 2200) / 200.0));
    }

    std::vector<std::span<const Sample>> planar{samples};
    meter.process(planar);
    meter.finish();

    CHECK(meter.samplePeakDbfsFor(0) == Approx(20.0 * std::log10(0.5)).margin(0.05));
    CHECK(meter.truePeakDbtpFor(0) == Approx(20.0 * std::log10(0.5)).margin(0.2));
}

TEST_CASE("true_peak: an Fs/4 tone with a 45-degree phase offset hides its peak exactly between "
          "samples",
          "[loudness]") {
    // The textbook deterministic inter-sample-peak signal: a full-scale tone at exactly a quarter
    // of the sample rate, phase-shifted by 45 degrees, samples at precisely +-cos(pi/4) = +-0.7071
    // (-3.01 dBFS) forever, while the continuous (band-limited-reconstructed) waveform's actual
    // peak of 1.0 (0 dBTP) falls exactly halfway between two samples every cycle. Unlike a generic
    // near-Nyquist tone at an arbitrary ratio (which can happen to sample close to its own peak),
    // this case is exact and rational-independent — every sample is identically +-0.7071.
    constexpr aud::SampleRate sampleRate = 48000;
    constexpr double          freq       = static_cast<double>(sampleRate) / 4.0;
    constexpr double          phase      = kPi / 4.0;

    std::vector<Sample> samples(4000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<Sample>(std::cos(2.0 * kPi * freq * static_cast<double>(i) / sampleRate + phase));
    }

    TruePeakMeter meter(sampleRate, 1, 4);
    std::vector<std::span<const Sample>> planar{samples};
    meter.process(planar);
    meter.finish();

    const double truePeakDb   = meter.truePeakDbtpFor(0);
    const double samplePeakDb = meter.samplePeakDbfsFor(0);

    CHECK(samplePeakDb == Approx(20.0 * std::log10(std::cos(kPi / 4.0))).margin(0.01));
    // 4x oversampling can under-read the true worst-case peak by up to ~0.6 dB (BS.1770-4 Annex
    // 2); allow for that plus the reconstruction filter's own small passband ripple.
    CHECK(truePeakDb > -1.0);
    CHECK(truePeakDb > samplePeakDb + 1.5);  // the true gap here is ~3 dB; require most of it back
    CHECK(meter.truePeakFrame() != aud::kNoFrame);
}

TEST_CASE("true_peak: overall peak matches the loudest channel", "[loudness]") {
    constexpr aud::SampleRate sampleRate = 48000;
    TruePeakMeter             meter(sampleRate, 2, 4);

    std::vector<Sample> quiet(2000, 0.1f);
    std::vector<Sample> loud(2000, 0.8f);
    std::vector<std::span<const Sample>> planar{quiet, loud};
    meter.process(planar);
    meter.finish();

    CHECK(meter.truePeakDbtpOverall() == Approx(meter.truePeakDbtpFor(1)).margin(1e-9));
}

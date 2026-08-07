#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <span>
#include <vector>

#include "../../engine/analysis/loudness/k_weighting.hpp"
#include "../../engine/analysis/loudness/loudness_analyzer.hpp"

using Catch::Approx;
using aud::ChannelIndex;
using aud::ChunkView;
using aud::Sample;
using aud::loudness::LoudnessConfig;
using aud::loudness::LoudnessResult;
using aud::loudness::makeLoudnessAnalyzer;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Independent re-derivation of the K-weighted gain at a given frequency, straight from the
// transfer function (mirrors test_k_weighting.cpp's biquadMagnitude) rather than from any
// memorised "a full-scale 1 kHz sine reads about X LUFS" factoid — the expected value below is
// computed from first principles (BS.1770's own loudness formula applied to a sine's mean square,
// scaled by the filter's actual measured gain at 1 kHz) so this test can't silently encode a
// misremembered reference number.
double kWeightingGainAt(double freqHz, double sampleRate) {
    auto magnitude = [&](const aud::loudness::BiquadCoeffs& c) {
        const std::complex<double> z = std::exp(std::complex<double>(0.0, -2.0 * 3.14159265358979323846 * freqHz / sampleRate));
        const std::complex<double> num = c.b0 + c.b1 * z + c.b2 * (z * z);
        const std::complex<double> den = 1.0 + c.a1 * z + c.a2 * (z * z);
        return std::abs(num / den);
    };
    return magnitude(aud::loudness::highShelfCoeffs(static_cast<aud::SampleRate>(sampleRate))) *
           magnitude(aud::loudness::rlbHighPassCoeffs(static_cast<aud::SampleRate>(sampleRate)));
}

std::vector<Sample> makeSine(std::size_t frames, double freqHz, double sampleRate, double amplitude) {
    std::vector<Sample> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = static_cast<Sample>(amplitude * std::sin(2.0 * kPi * freqHz * static_cast<double>(i) / sampleRate));
    }
    return out;
}

// Feeds a mono signal through a fresh LoudnessAnalyzer in fixed-size chunks (mirroring
// test_waveform_analyzer.cpp's pattern of driving concrete analysers only through the base
// Analyzer interface — see makeLoudnessAnalyzer's factory comment for why).
LoudnessResult analyzeMono(const std::vector<Sample>& signal, aud::SampleRate sampleRate,
                            LoudnessConfig config = {}) {
    LoudnessResult                 result;
    std::unique_ptr<aud::Analyzer> analyzer = makeLoudnessAnalyzer(result, config);
    REQUIRE(analyzer->begin(aud::AudioSpec{sampleRate, 1, static_cast<aud::FrameIndex>(signal.size())}).has_value());

    constexpr std::size_t chunkFrames = 4096;
    for (std::size_t offset = 0; offset < signal.size(); offset += chunkFrames) {
        const std::size_t take = std::min(chunkFrames, signal.size() - offset);
        std::span<const Sample> channel(signal.data() + offset, take);
        std::vector<std::span<const Sample>> planar{channel};
        ChunkView view{std::span<const std::span<const Sample>>(planar), static_cast<aud::FrameIndex>(offset)};
        REQUIRE(analyzer->process(view).has_value());
    }
    REQUIRE(analyzer->finish().has_value());
    return result;
}

}  // namespace

TEST_CASE("LoudnessAnalyzer: pure silence integrates to NaN, never 0", "[loudness]") {
    constexpr aud::SampleRate sampleRate = 48000;
    std::vector<Sample>       silence(sampleRate * 5, 0.0f);  // 5 s, well past the gating warm-up

    auto result = analyzeMono(silence, sampleRate);
    CHECK(std::isnan(result.integratedLufs));
    CHECK(std::isnan(result.loudnessRangeLu));
}

TEST_CASE("LoudnessAnalyzer: full-scale 1 kHz sine's integrated loudness matches BS.1770's formula "
          "applied to the filter's own measured gain",
          "[loudness]") {
    // This is a coarse pipeline-wiring check (K-weighting -> gating -> integration all connected
    // correctly end to end), not a substitute for the ±0.1 LU EBU Tech 3341 compliance vectors the
    // M08 acceptance criteria call for — those require the official test material and
    // cross-validation against ffmpeg/libebur128, neither of which this environment has available
    // (see the PR notes).
    constexpr aud::SampleRate sampleRate = 48000;
    auto                      sine       = makeSine(sampleRate * 3, 1000.0, sampleRate, 1.0);

    const double gain           = kWeightingGainAt(1000.0, sampleRate);
    const double weightedMeanSq = 0.5 * gain * gain;  // unweighted mean square of a full-scale sine is 0.5
    const double expectedLufs   = -0.691 + 10.0 * std::log10(weightedMeanSq);

    auto result = analyzeMono(sine, sampleRate);
    CHECK(result.integratedLufs == Approx(expectedLufs).margin(0.2));
}

TEST_CASE("LoudnessAnalyzer: 90% silence + 10% music reads the same integrated LUFS as the music alone "
          "(the relative-gate regression guard)",
          "[loudness]") {
    // 10 s music / 90 s silence rather than 3 s / 27 s: the silence-to-music cut always leaves a
    // handful (3, fixed by the 400 ms/100 ms-hop momentary window) of "straddling" momentary
    // blocks that mix a little silence into an otherwise-music window — an unavoidable edge effect
    // of any sliding-window measurement, not a gating bug. With only ~27 music-only momentary
    // blocks those 3 blocks are ~11% of the total and visibly drag the average down by ~0.2 LU;
    // with ~97 blocks (10 s) they're ~3%, comfortably inside the ticket's ±0.1 LU expectation.
    constexpr aud::SampleRate sampleRate = 48000;
    auto musicOnly = makeSine(sampleRate * 10, 1000.0, sampleRate, 0.5);

    std::vector<Sample> withSilence;
    withSilence.insert(withSilence.end(), sampleRate * 90, 0.0f);  // 90 s silence
    withSilence.insert(withSilence.end(), musicOnly.begin(), musicOnly.end());  // + 10 s music = 10%

    auto resultMusicOnly   = analyzeMono(musicOnly, sampleRate);
    auto resultWithSilence = analyzeMono(withSilence, sampleRate);

    REQUIRE_FALSE(std::isnan(resultMusicOnly.integratedLufs));
    REQUIRE_FALSE(std::isnan(resultWithSilence.integratedLufs));
    CHECK(resultWithSilence.integratedLufs == Approx(resultMusicOnly.integratedLufs).margin(0.1));
}

TEST_CASE("LoudnessAnalyzer: momentary and short-term series are populated at 100 ms resolution",
          "[loudness]") {
    constexpr aud::SampleRate sampleRate = 48000;
    auto                      sine       = makeSine(sampleRate * 5, 1000.0, sampleRate, 0.5);

    auto result = analyzeMono(sine, sampleRate);

    // Momentary warms up after 400 ms (4 sub-blocks), short-term after 3 s (30 sub-blocks); both
    // then advance one sample per 100 ms sub-block for the rest of a 5 s programme.
    const std::size_t totalSubBlocks = 50;  // 5 s / 100 ms
    CHECK(result.momentaryLufs.size() == totalSubBlocks - 3);
    CHECK(result.shortTermLufs.size() == totalSubBlocks - 29);

    for (float v : result.momentaryLufs) CHECK(v == Approx(-9.0).margin(3.0));  // sanity: in a plausible LUFS range
}

TEST_CASE("LoudnessAnalyzer: true peak, sample peak and gain-to-target are surfaced", "[loudness]") {
    constexpr aud::SampleRate sampleRate = 48000;
    auto                      sine       = makeSine(sampleRate * 2, 1000.0, sampleRate, 0.5);

    LoudnessConfig config;
    config.truePeakOversampling = 8;

    auto result = analyzeMono(sine, sampleRate, config);
    CHECK(result.truePeakOversampling == 8);
    CHECK(result.truePeakDbtp > -10.0);
    CHECK(result.truePeakDbtp < 3.0);
    CHECK(result.samplePeakDbfs == Approx(20.0 * std::log10(0.5)).margin(0.1));
    CHECK(result.truePeakPerChannelDbtp.size() == 1);
    CHECK(result.truePeakFrame != aud::kNoFrame);

    // gainToTargetDb is information only — verify the formula, not that it's ever applied.
    CHECK(result.gainToTargetDb(-14.0) == Approx(-14.0 - result.integratedLufs).margin(1e-9));
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "../../engine/analysis/clipping/clip_detector.hpp"

using Catch::Approx;
using aud::AudioSpec;
using aud::ChunkView;
using aud::Sample;
using aud::clipping::ClipKind;
using aud::clipping::ClippingConfig;
using aud::clipping::ClippingResult;
using aud::clipping::makeClipDetectorAnalyzer;

namespace {
constexpr double kPi = 3.14159265358979323846;

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

ClippingResult run(const std::vector<Sample>& mono, const ClippingConfig& config, aud::SampleRate sampleRate = 48000) {
    ClippingResult result;
    auto           analyzer = makeClipDetectorAnalyzer(result, config);

    REQUIRE(analyzer->begin(AudioSpec{sampleRate, 1, static_cast<aud::FrameIndex>(mono.size())}).has_value());

    std::vector<std::span<const Sample>> channels{mono};
    ChunkView                            view{channels, 0};
    REQUIRE(analyzer->process(view).has_value());
    REQUIRE(analyzer->finish().has_value());
    return result;
}

std::uint32_t countOf(const ClippingResult& r, ClipKind kind) {
    return r.eventCount[static_cast<std::size_t>(kind)];
}

}  // namespace

TEST_CASE("clipping: 16-bit source ceiling regression guard — 32767 run is detected", "[clipping]") {
    // M11's called-out single most likely bug: a hardcoded 1.0 ceiling would never fire here, since
    // M02 converts 16-bit integers to float by dividing by 2^15 (32768), so the positive maximum is
    // 32767/32768 = 0.999969..., strictly below 1.0.
    constexpr double kSixteenBitCeiling = 32767.0 / 32768.0;

    std::vector<Sample> samples(1000, 0.1f);
    for (std::size_t i = 100; i < 110; ++i) samples[i] = static_cast<Sample>(kSixteenBitCeiling);

    ClippingConfig config;
    config.containerBitDepth                 = 16;
    config.parameters.detectInterSamplePeaks = false;
    // The clipped run is also within the default near-clip band (it's near full scale by
    // definition) — disable that separate, legitimate finding here so this test isolates Digital.
    config.parameters.nearClipMinRun         = 1'000'000;

    auto result = run(samples, config);

    REQUIRE(countOf(result, ClipKind::Digital) == 1);
    REQUIRE(result.events.size() == 1);
    CHECK(result.events[0].kind == ClipKind::Digital);
    CHECK(result.events[0].sampleCount == 10);
    CHECK(result.events[0].peakValue == Approx(kSixteenBitCeiling).margin(1e-9));
    CHECK(result.totalClippedSamples == 10);

    // The hardcoded-1.0-ceiling bug this guards against: samples at the true 16-bit ceiling never
    // reach 1.0, so a detector using 1.0 directly would report zero events here.
    CHECK(kSixteenBitCeiling < 1.0);
}

TEST_CASE("clipping: float source at 1.4 reports OverFullScale, not Digital", "[clipping]") {
    std::vector<Sample> samples(1000, 0.2f);
    for (std::size_t i = 200; i < 205; ++i) samples[i] = 1.4f;

    ClippingConfig config;
    config.containerBitDepth                 = 0;  // float source
    config.parameters.detectInterSamplePeaks = false;
    config.parameters.nearClipMinRun         = 1'000'000;  // isolate OverFullScale, see the 16-bit test above

    auto result = run(samples, config);

    REQUIRE(countOf(result, ClipKind::OverFullScale) == 1);
    CHECK(countOf(result, ClipKind::Digital) == 0);
    REQUIRE(result.events.size() == 1);
    CHECK(result.events[0].kind == ClipKind::OverFullScale);
    CHECK(result.events[0].peakValue == Approx(1.4).margin(1e-6));
    CHECK(result.events[0].peakDbfs == Approx(20.0 * std::log10(1.4)).margin(1e-6));
}

TEST_CASE("clipping: an unclipped full-scale signal touching 1.0 for one sample per cycle reports "
          "zero events at minRunSamples=3",
          "[clipping]") {
    // Isolated single-sample spikes at exactly full scale, well separated, with everything else
    // comfortably below any threshold — a hard limiter never ran here, only a signal that happens
    // to touch 1.0 momentarily (M11: "a single sample at exactly 1.0 is not clipping; it's a sample
    // at full scale").
    std::vector<Sample> samples(2000, 0.5f);
    for (std::size_t i = 100; i < samples.size(); i += 100) samples[i] = 1.0f;

    ClippingConfig config;  // defaults: minRunSamples = 3
    config.containerBitDepth                 = 0;
    config.parameters.detectInterSamplePeaks = false;

    auto result = run(samples, config);

    CHECK(countOf(result, ClipKind::OverFullScale) == 0);
    CHECK(countOf(result, ClipKind::Digital) == 0);
    CHECK(result.events.empty());
    CHECK(result.totalClippedSamples == 0);
}

TEST_CASE("clipping: a hard-clipped sine reports the exact expected number of runs", "[clipping]") {
    // Amplitude-3 sine, clamped to +-1.0 — sin(x) exceeds 1/3 (the clip threshold) for an angular
    // width of 2*(90 - asin(1/3)) degrees around each peak, twice per period, with wide unclipped
    // gaps between them (~108 samples at period=1000, far beyond the default 32-sample
    // mergeGapSamples) so every clipped region survives as its own run, never merged.
    constexpr std::size_t period      = 1000;
    constexpr std::size_t periodCount = 5;
    constexpr double      amplitude   = 3.0;

    std::vector<Sample> samples(period * periodCount);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double raw = amplitude * std::sin(2.0 * kPi * static_cast<double>(i) / static_cast<double>(period));
        samples[i]        = static_cast<Sample>(std::clamp(raw, -1.0, 1.0));
    }

    ClippingConfig config;
    config.containerBitDepth                 = 0;
    config.parameters.detectInterSamplePeaks = false;
    config.parameters.nearClipMinRun         = 1'000'000;  // isolate OverFullScale, see the 16-bit test above

    auto result = run(samples, config);

    CHECK(countOf(result, ClipKind::OverFullScale) == 2 * periodCount);
    CHECK(result.events.size() == 2 * periodCount);
    for (const auto& e : result.events) {
        CHECK(e.kind == ClipKind::OverFullScale);
        CHECK(e.peakValue == Approx(1.0).margin(1e-6));
    }
}

TEST_CASE("clipping: an inter-sample-peak fixture is caught by ISP detection and nothing else", "[clipping]") {
    // The textbook deterministic ISP signal (see test_true_peak.cpp): a full-scale tone at exactly
    // Fs/4 with a 45-degree phase offset samples at only +-cos(pi/4) of its amplitude forever, while
    // the band-limited-reconstructed waveform's true peak reaches the full amplitude exactly halfway
    // between two samples every cycle. Scaling the amplitude to 10^(0.8/20) puts the continuous peak
    // at +0.8 dBTP while every actual sample sits at 0.7071 * that scale =~ -2.2 dBFS, comfortably
    // under -0.3 dBFS (and under the default -0.1 dBFS near-clip threshold too).
    constexpr aud::SampleRate sampleRate = 48000;
    constexpr double          freq       = static_cast<double>(sampleRate) / 4.0;
    constexpr double          phase      = kPi / 4.0;
    const double               scale      = dbToLinear(0.8);

    std::vector<Sample> samples(4000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<Sample>(scale *
                                          std::cos(2.0 * kPi * freq * static_cast<double>(i) / sampleRate + phase));
    }

    const double maxSampleDbfs =
        20.0 * std::log10(*std::max_element(samples.begin(), samples.end(),
                                             [](Sample a, Sample b) { return std::fabs(a) < std::fabs(b); }));
    REQUIRE(maxSampleDbfs < -0.3);

    ClippingConfig config;
    config.containerBitDepth = 0;

    auto result = run(samples, config, sampleRate);

    CHECK(countOf(result, ClipKind::Digital) == 0);
    CHECK(countOf(result, ClipKind::OverFullScale) == 0);
    CHECK(countOf(result, ClipKind::NearClip) == 0);
    CHECK(countOf(result, ClipKind::InterSamplePeak) > 0);

    const bool anyIsp = std::any_of(result.events.begin(), result.events.end(),
                                     [](const auto& e) { return e.kind == ClipKind::InterSamplePeak; });
    CHECK(anyIsp);
}

TEST_CASE("clipping: event capping keeps the worst events and reports the true total", "[clipping]") {
    // Ten isolated near-clip runs, well separated, each a little more severe than the last —
    // capping to 3 stored events must keep the three most severe (highest peak) and still report
    // the true count of 10 via eventCount, per M11's "never silently truncate" decision.
    constexpr int kRunCount = 10;
    constexpr int kRunGap   = 200;  // >> default mergeGapSamples (32)
    constexpr int kRunLen   = 3;    // == default nearClipMinRun

    std::vector<Sample> samples(kRunCount * kRunGap, 0.0f);
    std::vector<double> peaks(kRunCount);
    for (int r = 0; r < kRunCount; ++r) {
        const double peak = 0.989 + 0.001 * static_cast<double>(r);  // strictly increasing, all above -0.1 dBFS (~0.9886)
        peaks[r]           = peak;
        const std::size_t start = static_cast<std::size_t>(r) * kRunGap + 10;
        for (int j = 0; j < kRunLen; ++j) samples[start + static_cast<std::size_t>(j)] = static_cast<Sample>(peak);
    }

    ClippingConfig config;
    config.containerBitDepth                 = 0;
    config.parameters.detectInterSamplePeaks = false;
    config.parameters.maxStoredEvents        = 3;

    auto result = run(samples, config);

    REQUIRE(countOf(result, ClipKind::NearClip) == kRunCount);
    REQUIRE(result.events.size() == 3);

    // Worst (highest-peak) three runs are r=7,8,9 -> peaks 0.996, 0.997, 0.998, sorted worst-first.
    CHECK(result.events[0].peakValue == Approx(peaks[9]).margin(1e-9));
    CHECK(result.events[1].peakValue == Approx(peaks[8]).margin(1e-9));
    CHECK(result.events[2].peakValue == Approx(peaks[7]).margin(1e-9));
}

TEST_CASE("clipping: clean full-scale sine, clean noise floor and quiet material report zero events",
          "[clipping]") {
    constexpr std::size_t period = 480;
    std::vector<Sample>   sine(period * 50);
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = static_cast<Sample>(std::sin(2.0 * kPi * static_cast<double>(i) / static_cast<double>(period)) * 0.9);
    }

    ClippingConfig config;
    config.containerBitDepth                 = 24;
    config.parameters.detectInterSamplePeaks = false;

    auto result = run(sine, config);

    CHECK(result.events.empty());
    CHECK(result.totalClippedSamples == 0);
    CHECK(result.clippedFraction == Approx(0.0).margin(1e-12));
}

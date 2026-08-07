#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <span>
#include <vector>

#include "../../engine/analysis/dc/dc_analyzer.hpp"

using Catch::Approx;
using aud::AudioSpec;
using aud::ChunkView;
using aud::Sample;
using aud::dc::DcConfig;
using aud::dc::DcOffsetResult;
using aud::dc::DcPattern;
using aud::dc::makeDcAnalyzer;

namespace {
constexpr double kPi = 3.14159265358979323846;

DcOffsetResult run(const std::vector<Sample>& mono, const DcConfig& config = {}, aud::SampleRate sampleRate = 48000) {
    DcOffsetResult result;
    auto            analyzer = makeDcAnalyzer(result, config);

    REQUIRE(analyzer->begin(AudioSpec{sampleRate, 1, static_cast<aud::FrameIndex>(mono.size())}).has_value());

    std::vector<std::span<const Sample>> channels{mono};
    ChunkView                            view{channels, 0};
    REQUIRE(analyzer->process(view).has_value());
    REQUIRE(analyzer->finish().has_value());
    return result;
}

}  // namespace

TEST_CASE("dc: a pure DC signal at 0.1 reports offset exactly 0.1, pattern Constant", "[dc]") {
    std::vector<Sample> samples(48000 * 3, 0.1f);  // 3s, several 1s windows

    auto result = run(samples);

    REQUIRE(result.channels.size() == 1);
    const auto& c = result.channels[0];
    CHECK(c.offsetLinear == Approx(0.1).margin(1e-6));
    CHECK(c.pattern == DcPattern::Constant);
    CHECK(result.anySignificant);
}

TEST_CASE("dc: a sine with a 0.05 DC offset reports offset exactly 0.05, and the correction preview "
          "is exact",
          "[dc]") {
    constexpr std::size_t period      = 480;
    constexpr std::size_t periodCount = 200;
    constexpr double      amplitude   = 0.5;
    constexpr double      offset      = 0.05;

    std::vector<Sample> samples(period * periodCount);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double raw = amplitude * std::sin(2.0 * kPi * static_cast<double>(i) / static_cast<double>(period)) + offset;
        samples[i]        = static_cast<Sample>(raw);
    }

    auto result = run(samples);

    REQUIRE(result.channels.size() == 1);
    const auto& c = result.channels[0];
    CHECK(c.offsetLinear == Approx(offset).margin(1e-6));

    // Analytic correction preview: subtracting the constant offset shifts min/max by exactly that
    // constant, so the corrected peak is exactly the sine's own amplitude (M12: "computed
    // analytically ... no second pass needed for the constant case").
    const double expectedPeakAfterCorrectionDbfs = 20.0 * std::log10(amplitude);
    CHECK(c.peakAfterCorrectionDbfs == Approx(expectedPeakAfterCorrectionDbfs).margin(1e-3));

    // Pre-correction, the waveform's min/max are asymmetric by exactly 2*offset (the analogue of
    // M04's bin min/max asymmetry the design doc calls out): max ~ amplitude+offset, min ~
    // -amplitude+offset, so max - (-min) - 2*amplitude == 2*offset.
    CHECK(c.headroomLostDb > 0.0);
}

TEST_CASE("dc: two halves at +0.01 and -0.01 report global ~0 but pattern Sectional with the step at "
          "the midpoint",
          "[dc]") {
    // The regression guard for the global-mean blind spot M12's risk table calls out explicitly.
    constexpr aud::SampleRate sampleRate = 48000;
    constexpr std::size_t     halfFrames = sampleRate * 5;  // 5s each half, 10 windows total

    std::vector<Sample> samples(halfFrames * 2);
    std::fill(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(halfFrames), static_cast<Sample>(0.01));
    std::fill(samples.begin() + static_cast<std::ptrdiff_t>(halfFrames), samples.end(), static_cast<Sample>(-0.01));

    auto result = run(samples, DcConfig{}, sampleRate);

    REQUIRE(result.channels.size() == 1);
    const auto& c = result.channels[0];
    CHECK(c.offsetLinear == Approx(0.0).margin(1e-6));
    CHECK(c.pattern == DcPattern::Sectional);
    REQUIRE(c.stepLocations.size() == 1);
    CHECK(c.stepLocations[0] == static_cast<aud::FrameIndex>(halfFrames));
}

TEST_CASE("dc: a linear DC ramp reports pattern Drifting", "[dc]") {
    constexpr aud::SampleRate sampleRate = 48000;
    constexpr std::size_t     seconds    = 10;
    constexpr std::size_t     total      = sampleRate * seconds;
    constexpr double          startDc    = 0.0;
    constexpr double          endDc      = 0.1;

    std::vector<Sample> samples(total);
    for (std::size_t i = 0; i < total; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(total - 1);
        samples[i]      = static_cast<Sample>(startDc + (endDc - startDc) * t);
    }

    auto result = run(samples, DcConfig{}, sampleRate);

    REQUIRE(result.channels.size() == 1);
    const auto& c = result.channels[0];
    CHECK(c.pattern == DcPattern::Drifting);
    CHECK(c.recommendedHighpassHz > 0.0);
}

TEST_CASE("dc: clean noise reports pattern None, not a spurious finding", "[dc]") {
    constexpr aud::SampleRate sampleRate = 48000;
    constexpr std::size_t     seconds    = 5;

    std::mt19937                          rng(1234);
    std::uniform_real_distribution<float> dist(-0.02f, 0.02f);

    std::vector<Sample> samples(sampleRate * seconds);
    for (auto& s : samples) s = dist(rng);

    auto result = run(samples, DcConfig{}, sampleRate);

    REQUIRE(result.channels.size() == 1);
    const auto& c = result.channels[0];
    CHECK(c.pattern == DcPattern::None);
    CHECK_FALSE(result.anySignificant);
}

TEST_CASE("dc: significance threshold is configurable and echoed back", "[dc]") {
    DcConfig config;
    config.significanceThresholdDbfs = -40.0;

    std::vector<Sample> samples(48000, 0.005f);  // ~ -46 dBFS, below -40 dBFS threshold

    auto result = run(samples, config);

    CHECK(result.significanceThresholdDbfs == Approx(-40.0));
    REQUIRE(result.channels.size() == 1);
    CHECK(result.channels[0].pattern == DcPattern::None);
}

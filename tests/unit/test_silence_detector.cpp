#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "../../engine/analysis/silence/silence_detector.hpp"

using Catch::Approx;
using aud::FrameIndex;
using aud::SampleRate;
using aud::silence::ChannelMode;
using aud::silence::SilenceDetector;
using aud::silence::SilenceInput;
using aud::silence::SilenceKind;
using aud::silence::SilenceParameters;
using aud::silence::SilencePosition;
using aud::silence::SilenceResult;

namespace {

constexpr SampleRate kSampleRate = 48000;
constexpr double      kWindowSeconds = 0.05;
constexpr std::size_t kWindowFrames  = 2400;  // 48000 / 20

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

// Builds a mono SilenceInput from a sequence of per-window dBFS levels (converted to linear RMS).
SilenceInput makeMonoInput(const std::vector<double>& windowLevelsDb) {
    SilenceInput input;
    input.channelCount     = 1;
    input.sampleRate        = kSampleRate;
    input.rmsWindowSeconds = kWindowSeconds;
    input.rmsSeries.reserve(windowLevelsDb.size());
    for (double db : windowLevelsDb) {
        input.rmsSeries.push_back(static_cast<float>(std::isinf(db) ? 0.0 : dbToLinear(db)));
    }
    input.frameCount = static_cast<FrameIndex>(windowLevelsDb.size() * kWindowFrames);
    return input;
}

// Builds a stereo SilenceInput, interleaved [L,R] per window, from per-channel dBFS levels.
SilenceInput makeStereoInput(const std::vector<double>& leftDb, const std::vector<double>& rightDb) {
    REQUIRE(leftDb.size() == rightDb.size());
    SilenceInput input;
    input.channelCount     = 2;
    input.sampleRate        = kSampleRate;
    input.rmsWindowSeconds = kWindowSeconds;
    input.rmsSeries.reserve(leftDb.size() * 2);
    for (std::size_t i = 0; i < leftDb.size(); ++i) {
        input.rmsSeries.push_back(static_cast<float>(std::isinf(leftDb[i]) ? 0.0 : dbToLinear(leftDb[i])));
        input.rmsSeries.push_back(static_cast<float>(std::isinf(rightDb[i]) ? 0.0 : dbToLinear(rightDb[i])));
    }
    input.frameCount = static_cast<FrameIndex>(leftDb.size() * kWindowFrames);
    return input;
}

constexpr double kLoud   = -6.0;    // clearly "not silent"
constexpr double kQuiet  = -80.0;   // clearly below thresholdDb=-60

}  // namespace

TEST_CASE("silence: pure digital silence produces one EntireFile region", "[silence]") {
    constexpr std::size_t windows = 100;
    SilenceInput input = makeMonoInput(std::vector<double>(windows, -std::numeric_limits<double>::infinity()));
    input.digitalSilenceSeries.assign(windows, 1);

    SilenceParameters params;
    auto result = SilenceDetector::detectDigital(input, params);

    REQUIRE(result.regions.size() == 1);
    CHECK(result.regions[0].position == SilencePosition::EntireFile);
    CHECK(result.regions[0].kind == SilenceKind::Digital);
    CHECK(result.silenceFraction == Approx(1.0).margin(1e-9));
}

TEST_CASE("silence: tone-silence-tone with an exact 2.000s gap yields one internal region", "[silence]") {
    // 40 windows * 50ms = 2.000s exactly.
    std::vector<double> levels;
    for (int i = 0; i < 20; ++i) levels.push_back(kLoud);
    for (int i = 0; i < 40; ++i) levels.push_back(kQuiet);
    for (int i = 0; i < 20; ++i) levels.push_back(kLoud);

    SilenceInput input = makeMonoInput(levels);
    SilenceParameters params;  // defaults: -60dBFS, 500ms min, 100ms merge, hysteresis on

    auto result = SilenceDetector::detectThreshold(input, params);

    REQUIRE(result.regions.size() == 1);
    const auto& region = result.regions[0];
    CHECK(region.position == SilencePosition::Internal);
    CHECK(region.endSeconds - region.startSeconds == Approx(2.000).margin(0.001));
}

TEST_CASE("silence: a single loud click inside a long silence does not split the region", "[silence]") {
    // 10s of silence (200 windows) with one loud window in the middle.
    std::vector<double> levels(200, kQuiet);
    levels[100] = kLoud;  // a 50ms "click" — shorter than mergeGapMs's default 100ms

    SilenceInput input = makeMonoInput(levels);
    SilenceParameters params;  // mergeGapMs default 100ms bridges this

    auto result = SilenceDetector::detectThreshold(input, params);

    REQUIRE(result.regions.size() == 1);
    CHECK(result.regions[0].startSeconds == Approx(0.0).margin(1e-9));
    CHECK(result.regions[0].endSeconds == Approx(10.0).margin(1e-6));
}

TEST_CASE("silence: material dithering around the threshold produces one region with hysteresis", "[silence]") {
    // Bounces between -58dB and -63dB, i.e. always <= thresholdDb+hysteresisDb (-57), but crosses
    // the bare -60dB threshold constantly. Long enough to survive minDurationMs.
    std::vector<double> levels(200);
    for (std::size_t i = 0; i < levels.size(); ++i) levels[i] = (i % 2 == 0) ? -58.0 : -63.0;

    // Isolate hysteresis's effect from merge/min-duration, which would otherwise bridge the very
    // gaps this test is checking hysteresis alone prevents.
    SilenceInput input = makeMonoInput(levels);
    SilenceParameters params;
    params.useHysteresis = true;
    params.hysteresisDb   = 3.0;  // exit at -57dB, never crossed
    params.mergeGapMs     = 0.0;
    params.minDurationMs  = 0.0;

    auto result = SilenceDetector::detectThreshold(input, params);
    REQUIRE(result.regions.size() == 1);

    // Without hysteresis, the same material produces a marker storm.
    SilenceParameters noHysteresis = params;
    noHysteresis.useHysteresis      = false;
    auto stormy = SilenceDetector::detectThreshold(input, noHysteresis);
    CHECK(stormy.regions.size() > 10);
}

TEST_CASE("silence: a run shorter than minDurationMs is not reported", "[silence]") {
    std::vector<double> levels;
    for (int i = 0; i < 20; ++i) levels.push_back(kLoud);
    for (int i = 0; i < 4; ++i) levels.push_back(kQuiet);  // 200ms, below the 500ms default minimum
    for (int i = 0; i < 20; ++i) levels.push_back(kLoud);

    SilenceInput input = makeMonoInput(levels);
    SilenceParameters params;  // default minDurationMs = 500ms

    auto result = SilenceDetector::detectThreshold(input, params);
    CHECK(result.regions.empty());
}

TEST_CASE("silence: leading, trailing and internal regions classify correctly", "[silence]") {
    std::vector<double> levels;
    for (int i = 0; i < 20; ++i) levels.push_back(kQuiet);   // leading
    for (int i = 0; i < 20; ++i) levels.push_back(kLoud);
    for (int i = 0; i < 20; ++i) levels.push_back(kQuiet);   // internal
    for (int i = 0; i < 20; ++i) levels.push_back(kLoud);
    for (int i = 0; i < 20; ++i) levels.push_back(kQuiet);   // trailing

    SilenceInput input = makeMonoInput(levels);
    SilenceParameters params;

    auto result = SilenceDetector::detectThreshold(input, params);
    REQUIRE(result.regions.size() == 3);
    CHECK(result.regions[0].position == SilencePosition::Leading);
    CHECK(result.regions[1].position == SilencePosition::Internal);
    CHECK(result.regions[2].position == SilencePosition::Trailing);
    CHECK(result.leadingSilenceSeconds == Approx(1.0).margin(1e-6));
    CHECK(result.trailingSilenceSeconds == Approx(1.0).margin(1e-6));
}

TEST_CASE("silence: channelMode All vs Any when only the left channel is silent", "[silence]") {
    std::vector<double> leftDb(200, kQuiet);
    std::vector<double> rightDb(200, kLoud);

    SilenceInput input = makeStereoInput(leftDb, rightDb);

    SilenceParameters allMode;
    allMode.channelMode = ChannelMode::All;
    auto allResult = SilenceDetector::detectThreshold(input, allMode);
    CHECK(allResult.regions.empty());  // right channel is never silent, so "All" never triggers

    SilenceParameters anyMode;
    anyMode.channelMode = ChannelMode::Any;
    auto anyResult = SilenceDetector::detectThreshold(input, anyMode);
    REQUIRE(anyResult.regions.size() == 1);
    CHECK(anyResult.regions[0].channelMask == 0b01u);  // only channel 0 (left) ever silent
}

TEST_CASE("silence: perceptual mode gates at the supplied LUFS threshold", "[silence]") {
    std::vector<float> momentary(200, -80.0f);  // well below the -70 LUFS default gate

    SilenceInput input;
    input.sampleRate                 = kSampleRate;
    input.momentaryLufs               = momentary;
    input.momentaryLufsWindowSeconds = 0.1;
    input.frameCount                  = static_cast<FrameIndex>(momentary.size() * kSampleRate / 10);

    SilenceParameters params;
    auto result = SilenceDetector::detectPerceptual(input, params);

    REQUIRE(result.regions.size() == 1);
    CHECK(result.regions[0].kind == SilenceKind::Perceptual);
    CHECK(result.regions[0].position == SilencePosition::EntireFile);
}

TEST_CASE("silence: parameters are echoed back into the result", "[silence]") {
    SilenceInput input = makeMonoInput(std::vector<double>(50, kQuiet));
    SilenceParameters params;
    params.thresholdDb = -50.0;
    params.minDurationMs = 250.0;

    auto result = SilenceDetector::detectThreshold(input, params);
    CHECK(result.parametersUsed.thresholdDb == Approx(-50.0));
    CHECK(result.parametersUsed.minDurationMs == Approx(250.0));
}

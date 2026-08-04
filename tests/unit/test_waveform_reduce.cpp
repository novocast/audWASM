#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <span>
#include <vector>

#include "../../engine/waveform/reduce.hpp"
#include "../../engine/waveform/waveform_bin.hpp"

using aud::Sample;
using aud::waveform::kBaseBinFrames;
using aud::waveform::reduceOneBin;
using aud::waveform::reduceToBins;
using aud::waveform::WaveformBin;
using Catch::Approx;

namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

TEST_CASE("reduceOneBin on an empty span yields a zeroed bin", "[waveform]") {
    WaveformBin bin = reduceOneBin(std::span<const Sample>{});
    REQUIRE(bin.min == 0.0f);
    REQUIRE(bin.max == 0.0f);
    REQUIRE(bin.rms == 0.0f);
    REQUIRE(bin.absPeak == 0.0f);
}

TEST_CASE("reduceOneBin on digital silence is exactly zero", "[waveform]") {
    std::vector<Sample> silence(kBaseBinFrames, 0.0f);
    WaveformBin          bin = reduceOneBin(silence);
    REQUIRE(bin.min == 0.0f);
    REQUIRE(bin.max == 0.0f);
    REQUIRE(bin.rms == 0.0f);
    REQUIRE(bin.absPeak == 0.0f);
}

TEST_CASE("a full-scale sine produces max~+1, min~-1, rms~0.7071 in every bin", "[waveform]") {
    constexpr std::size_t kPeriod = 32;  // divides kBaseBinFrames exactly: 8 whole periods/bin
    constexpr std::size_t kBins   = 10;

    std::vector<Sample> sine(kBaseBinFrames * kBins);
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = static_cast<Sample>(std::sin(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kPeriod)));
    }

    std::vector<WaveformBin> bins;
    reduceToBins(sine, kBaseBinFrames, bins);
    REQUIRE(bins.size() == kBins);
    for (const auto& bin : bins) {
        REQUIRE(bin.max == Approx(1.0).margin(0.001));
        REQUIRE(bin.min == Approx(-1.0).margin(0.001));
        REQUIRE(bin.rms == Approx(0.70710678).margin(0.001));
        REQUIRE(bin.absPeak == Approx(1.0).margin(0.001));
    }
}

TEST_CASE("a full-scale square wave produces max~+1, min~-1, rms~1.0 in every bin", "[waveform]") {
    constexpr std::size_t kPeriod = 32;  // divides kBaseBinFrames exactly: 8 whole periods/bin
    constexpr std::size_t kBins   = 10;

    std::vector<Sample> square(kBaseBinFrames * kBins);
    for (std::size_t i = 0; i < square.size(); ++i) {
        square[i] = (i % kPeriod) < (kPeriod / 2) ? 1.0f : -1.0f;
    }

    std::vector<WaveformBin> bins;
    reduceToBins(square, kBaseBinFrames, bins);
    REQUIRE(bins.size() == kBins);
    for (const auto& bin : bins) {
        REQUIRE(bin.max == 1.0f);
        REQUIRE(bin.min == -1.0f);
        REQUIRE(bin.rms == Approx(1.0).margin(0.001));
        REQUIRE(bin.absPeak == 1.0f);
    }
}

TEST_CASE("an impulse appears in exactly one bin, at index n / kBaseBinFrames", "[waveform]") {
    constexpr std::size_t kTotalFrames  = kBaseBinFrames * 8;
    constexpr std::size_t kImpulseFrame = kBaseBinFrames * 3 + 17;

    std::vector<Sample> samples(kTotalFrames, 0.0f);
    samples[kImpulseFrame] = 1.0f;

    std::vector<WaveformBin> bins;
    reduceToBins(samples, kBaseBinFrames, bins);

    const std::size_t expectedBin = kImpulseFrame / kBaseBinFrames;
    for (std::size_t i = 0; i < bins.size(); ++i) {
        if (i == expectedBin) {
            REQUIRE(bins[i].max == 1.0f);
            REQUIRE(bins[i].absPeak == 1.0f);
        } else {
            REQUIRE(bins[i].max == 0.0f);
            REQUIRE(bins[i].min == 0.0f);
            REQUIRE(bins[i].absPeak == 0.0f);
        }
    }
}

TEST_CASE("a DC-offset signal renders asymmetric min/max, not mirror images", "[waveform]") {
    constexpr double kDc        = 0.5;
    constexpr double kAmplitude = 0.1;

    std::vector<Sample> samples(kBaseBinFrames);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<Sample>(kDc + kAmplitude * std::sin(2.0 * kPi * static_cast<double>(i) / 16.0));
    }

    WaveformBin bin = reduceOneBin(samples);
    REQUIRE(bin.min > 0.0f);               // the offset keeps it from ever crossing zero
    REQUIRE(bin.max > bin.min);
    REQUIRE(bin.min != Approx(-bin.max));  // not a mirror image around zero — DC is visible
}

TEST_CASE("reduceToBins produces a correctly-sized trailing partial bin", "[waveform]") {
    std::vector<Sample> samples(kBaseBinFrames * 2 + 10, 1.0f);

    std::vector<WaveformBin> bins;
    reduceToBins(samples, kBaseBinFrames, bins);
    REQUIRE(bins.size() == 3);
    REQUIRE(bins[0].max == 1.0f);
    REQUIRE(bins[1].max == 1.0f);
    REQUIRE(bins[2].max == 1.0f);  // partial (10-frame) bin still reduced correctly
    REQUIRE(bins[2].rms == 1.0f);
}

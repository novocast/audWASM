#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "../../engine/spectrogram/point_query.hpp"
#include "../../engine/spectrogram/tile.hpp"
#include "../../engine/util/audio_buffer.hpp"

using aud::AudioBuffer;
using aud::Sample;
using aud::spectrogram::queryPoint;
using aud::spectrogram::TileConfig;
using Catch::Approx;

namespace {

constexpr aud::SampleRate kSampleRate = 44100;

AudioBuffer makeSine(double hz, double seconds) {
    auto result = AudioBuffer::create(kSampleRate, 1);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();

    const auto          frameCount = static_cast<std::size_t>(seconds * kSampleRate);
    std::vector<Sample> samples(frameCount);
    for (std::size_t i = 0; i < frameCount; ++i) {
        samples[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSampleRate));
    }
    std::vector<std::span<const Sample>> planar{samples};
    REQUIRE(buffer.append(planar, frameCount).has_value());
    return buffer;
}

}  // namespace

TEST_CASE("queryPoint: reports a pure tone's frequency within 0.5 Hz at fftSize 4096",
          "[spectrogram][point_query]") {
    // Deliberately not bin-aligned (bin width at 4096/44100 is ~10.77 Hz) so this actually exercises
    // peak interpolation rather than happening to land exactly on a bin centre.
    constexpr double kToneHz = 1234.5;
    auto             buffer  = makeSine(kToneHz, 1.0);

    TileConfig config;
    config.fftSize = 4096;
    config.window  = 1;  // Hann
    config.scaling = 1;  // Amplitude

    auto result = queryPoint(buffer, 0, /*timeSeconds=*/0.5, /*targetHz=*/kToneHz, config);
    REQUIRE(result.has_value());
    REQUIRE(result.value().frequencyHz == Approx(kToneHz).margin(0.5));
    // A full-scale sine should read close to 0 dBFS (M06's Amplitude-scaling convention).
    REQUIRE(result.value().magnitudeDb > -1.0);
}

TEST_CASE("queryPoint: still finds the peak when targetHz is a few bins off", "[spectrogram][point_query]") {
    constexpr double kToneHz = 2000.0;
    auto              buffer = makeSine(kToneHz, 1.0);

    TileConfig config;
    config.fftSize = 4096;

    auto result = queryPoint(buffer, 0, 0.5, kToneHz + 25.0, config);
    REQUIRE(result.has_value());
    REQUIRE(result.value().frequencyHz == Approx(kToneHz).margin(0.5));
}

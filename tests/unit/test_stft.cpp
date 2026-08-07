#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "../../engine/fft/stft.hpp"

using aud::FrameIndex;
using aud::fft::SpectrumScaling;
using aud::fft::StftConfig;
using aud::fft::StftFrame;
using aud::fft::StftProcessor;
using aud::fft::WindowType;
using Catch::Approx;

TEST_CASE("StftProcessor: rejects inconsistent configs", "[fft][stft]") {
    StftConfig config;
    config.fftSize = 0;
    REQUIRE_FALSE(StftProcessor::create(config, 44100).has_value());

    config          = StftConfig{};
    config.hopSize  = 4096;  // > windowSize
    REQUIRE_FALSE(StftProcessor::create(config, 44100).has_value());

    config              = StftConfig{};
    config.zeroPadded   = true;
    config.windowSize   = config.fftSize;  // zeroPadded but no actual padding
    REQUIRE_FALSE(StftProcessor::create(config, 44100).has_value());

    REQUIRE_FALSE(StftProcessor::create(StftConfig{}, 0).has_value());
    REQUIRE(StftProcessor::create(StftConfig{}, 44100).has_value());
}

TEST_CASE("StftProcessor: impulse produces its peak in the frame nearest its time (centring)", "[fft][stft]") {
    StftConfig config;
    config.fftSize  = 1024;
    config.hopSize  = 256;
    config.window   = WindowType::Hann;
    config.scaling  = SpectrumScaling::Raw;
    config.centered = true;

    auto created = StftProcessor::create(config, 44100);
    REQUIRE(created.has_value());
    auto& stft = created.value();

    constexpr std::size_t totalSamples = 8000;
    constexpr std::size_t impulseAt    = 4000;
    std::vector<float>    signal(totalSamples, 0.0f);
    signal[impulseAt] = 1.0f;

    std::vector<std::pair<std::size_t, double>> frameEnergies;
    auto                                        callback = [&](const StftFrame& frame) {
        double energy = 0.0;
        for (float b : frame.bins) {
            energy += static_cast<double>(b) * b;
        }
        frameEnergies.emplace_back(frame.frameIndex, energy);
    };

    REQUIRE(stft.process(signal, callback).has_value());
    REQUIRE(stft.finish(callback).has_value());
    REQUIRE_FALSE(frameEnergies.empty());

    const auto peak = std::max_element(frameEnergies.begin(), frameEnergies.end(),
                                        [](const auto& a, const auto& b) { return a.second < b.second; });

    const double impulseTime = static_cast<double>(impulseAt) / 44100.0;
    std::size_t  bestFrame   = 0;
    double       bestDelta   = 1e9;
    for (const auto& [frameIndex, energy] : frameEnergies) {
        const double delta = std::abs(stft.frameTimeSeconds(frameIndex) - impulseTime);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestFrame = frameIndex;
        }
    }

    REQUIRE(peak->first == bestFrame);
}

TEST_CASE("StftProcessor: frameCount() matches what process()+finish() actually emits", "[fft][stft]") {
    for (bool centered : {true, false}) {
        CAPTURE(centered);
        StftConfig config;
        config.fftSize  = 512;
        config.hopSize  = 128;
        config.centered = centered;

        auto created = StftProcessor::create(config, 44100);
        REQUIRE(created.has_value());
        auto& stft = created.value();

        constexpr std::size_t totalSamples = 5000;
        std::vector<float>    signal(totalSamples, 0.1f);

        std::size_t emitted  = 0;
        auto        callback = [&](const StftFrame&) { ++emitted; };
        REQUIRE(stft.process(signal, callback).has_value());
        REQUIRE(stft.finish(callback).has_value());

        REQUIRE(emitted == stft.frameCount(static_cast<FrameIndex>(totalSamples)));
    }
}

TEST_CASE("StftProcessor: binFrequencyHz/frameTimeSeconds are correct for odd fftSize", "[fft][stft]") {
    for (bool centered : {true, false}) {
        CAPTURE(centered);
        StftConfig config;
        config.fftSize  = 45;  // odd, factors 3^2*5 — supported, and exercises integer-division edges
        config.hopSize  = 9;
        config.centered = centered;

        auto created = StftProcessor::create(config, 1000);
        REQUIRE(created.has_value());
        auto& stft = created.value();

        REQUIRE(stft.binCount() == 45 / 2 + 1);
        REQUIRE(stft.binFrequencyHz(0) == Approx(0.0));
        REQUIRE(stft.binFrequencyHz(1) == Approx(1000.0 / 45.0));

        const double centerOffset = centered ? 0.0 : 45.0 / 2.0;
        for (std::size_t frameIdx : {std::size_t{0}, std::size_t{1}, std::size_t{5}}) {
            const double expected = (static_cast<double>(frameIdx) * 9.0 + centerOffset) / 1000.0;
            REQUIRE(stft.frameTimeSeconds(frameIdx) == Approx(expected));
        }
    }
}

TEST_CASE("StftProcessor: throughput over 5 seconds of audio at 2048/512", "[fft][stft][.benchmark]") {
    StftConfig config;
    config.fftSize = 2048;
    config.hopSize = 512;

    auto created = StftProcessor::create(config, 44100);
    REQUIRE(created.has_value());
    auto& stft = created.value();

    std::vector<float> signal(44100 * 5);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
    }

    auto callback = [](const StftFrame&) {};

    BENCHMARK("STFT process() over 5s of audio") { return stft.process(signal, callback); };
}

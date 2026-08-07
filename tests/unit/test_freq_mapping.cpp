#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "../../engine/spectrogram/frame_computer.hpp"
#include "../../engine/spectrogram/freq_mapping.hpp"
#include "../../engine/util/audio_buffer.hpp"

using aud::AudioBuffer;
using aud::Sample;
using aud::fft::SpectrumScaling;
using aud::fft::WindowType;
using aud::spectrogram::FrameComputer;
using aud::spectrogram::FreqAxis;
using aud::spectrogram::FreqMapping;
using Catch::Approx;

namespace {

constexpr aud::SampleRate kSampleRate = 44100;
constexpr std::size_t     kFftSize    = 4096;

AudioBuffer makeSine(double hz, double seconds) {
    auto result = AudioBuffer::create(kSampleRate, 1);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();

    const auto         frameCount = static_cast<std::size_t>(seconds * kSampleRate);
    std::vector<Sample> samples(frameCount);
    for (std::size_t i = 0; i < frameCount; ++i) {
        samples[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSampleRate));
    }
    std::vector<std::span<const Sample>> planar{samples};
    REQUIRE(buffer.append(planar, frameCount).has_value());
    return buffer;
}

// Returns the row index with the highest dB value.
std::size_t argmaxRow(const FreqMapping& mapping, std::span<const double> db) {
    std::size_t best = 0;
    for (std::size_t row = 1; row < mapping.rowCount(); ++row) {
        if (db[row] > db[best]) {
            best = row;
        }
    }
    return best;
}

}  // namespace

TEST_CASE("FreqMapping: a 1kHz sine produces a single bright row at the correct frequency, on every axis",
          "[spectrogram][freq_mapping]") {
    constexpr double kToneHz = 1000.0;
    auto              buffer = makeSine(kToneHz, 1.0);

    auto computerResult = FrameComputer::create(kFftSize, WindowType::Hann, SpectrumScaling::Amplitude, kSampleRate);
    REQUIRE(computerResult.has_value());
    auto& computer = computerResult.value();

    const auto magnitudes = computer.computeFrame(buffer, 0, buffer.frameCount() / 2);

    for (FreqAxis axis : {FreqAxis::Linear, FreqAxis::Log, FreqAxis::Mel, FreqAxis::Bark}) {
        auto mappingResult = FreqMapping::create(axis, 20.0, kSampleRate, computer.binCount());
        REQUIRE(mappingResult.has_value());
        auto& mapping = mappingResult.value();

        std::vector<double> db(mapping.rowCount());
        mapping.mapToDb(magnitudes, db);

        const std::size_t peakRow = argmaxRow(mapping, db);
        const double      peakHz  = mapping.row(peakRow).centerHz;

        CAPTURE(static_cast<int>(axis));
        // Row width varies with axis and position; a couple of bins' worth of tolerance around the
        // resolved row is enough to confirm "a single bright row at the correct frequency" without
        // being sensitive to exactly which row's centre the tone falls closest to.
        REQUIRE(peakHz == Approx(kToneHz).margin(60.0));

        // "Single bright row": rows a few steps away from the peak should be meaningfully dimmer.
        // Immediate neighbours can legitimately stay bright too — the Hann window's main lobe
        // spans a few FFT bins, and axes with fine row spacing near 1kHz (Mel/Bark compress rows
        // more as frequency rises, so several adjacent rows can map to the same or adjacent bins)
        // will show that spread as more than one bright row right next to the peak.
        constexpr std::size_t kNeighbourhood = 3;
        for (std::size_t row = 0; row < mapping.rowCount(); ++row) {
            const std::size_t delta = row > peakRow ? row - peakRow : peakRow - row;
            if (delta <= kNeighbourhood) continue;
            REQUIRE(db[row] < db[peakRow] - 3.0);
        }
    }
}

TEST_CASE("FreqMapping: Log/Mel/Bark axes span [minHz, nyquist] with monotonically increasing row centres",
          "[spectrogram][freq_mapping]") {
    constexpr double kMinHz = 20.0;
    const auto       binCount = kFftSize / 2 + 1;

    for (FreqAxis axis : {FreqAxis::Log, FreqAxis::Mel, FreqAxis::Bark}) {
        auto mappingResult = FreqMapping::create(axis, kMinHz, kSampleRate, binCount);
        REQUIRE(mappingResult.has_value());
        auto& mapping = mappingResult.value();

        // Endpoints: the log-style axis should start near minHz and end near Nyquist.
        // Row *centres* sit half a row-width inside the [minHz, nyquist] edges the mapping actually
        // spans; on a compressive axis the top row's width (in Hz) can be large, so the margin here
        // is generous by design — this is checking "close to the edge", not "exactly the edge".
        REQUIRE(mapping.row(0).centerHz == Approx(kMinHz).margin(kMinHz));
        REQUIRE(mapping.row(mapping.rowCount() - 1).centerHz == Approx(kSampleRate / 2.0).margin(900.0));

        for (std::size_t row = 1; row < mapping.rowCount(); ++row) {
            REQUIRE(mapping.row(row).centerHz > mapping.row(row - 1).centerHz);
        }
    }

    // Linear axis should have (nearly) uniform Hz spacing between successive row centres, in
    // contrast to Log/Mel/Bark's compressed-at-the-top spacing.
    auto linearResult = FreqMapping::create(FreqAxis::Linear, kMinHz, kSampleRate, binCount);
    REQUIRE(linearResult.has_value());
    auto&        linear     = linearResult.value();
    const double firstDelta = linear.row(1).centerHz - linear.row(0).centerHz;
    const double lastDelta =
        linear.row(linear.rowCount() - 1).centerHz - linear.row(linear.rowCount() - 2).centerHz;
    REQUIRE(lastDelta == Approx(firstDelta).epsilon(0.05));

    auto logResult = FreqMapping::create(FreqAxis::Log, kMinHz, kSampleRate, binCount);
    REQUIRE(logResult.has_value());
    auto&        log       = logResult.value();
    const double logFirst  = log.row(1).centerHz - log.row(0).centerHz;
    const double logLast   = log.row(log.rowCount() - 1).centerHz - log.row(log.rowCount() - 2).centerHz;
    // Log axis: spacing near the top must be much larger than spacing near the bottom (that's the
    // whole point of a log axis — high rows cover many bins).
    REQUIRE(logLast > logFirst * 10.0);
}

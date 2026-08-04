#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "../../engine/playback/resampler.hpp"

using aud::ChannelIndex;
using aud::Sample;
using aud::SampleRate;
using aud::playback::Resampler;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<Sample> makeSine(double frequencyHz, SampleRate rate, std::size_t frames) {
    std::vector<Sample> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = static_cast<Sample>(std::sin(2.0 * kPi * frequencyHz * static_cast<double>(i) / static_cast<double>(rate)));
    }
    return out;
}

// Single-bin (Goertzel) DFT magnitude of `signal` at `frequencyHz` for a signal sampled at `rate`.
double goertzelMagnitude(const std::vector<Sample>& signal, double frequencyHz, SampleRate rate) {
    const auto   n = static_cast<double>(signal.size());
    const double k = std::round(n * frequencyHz / static_cast<double>(rate));
    const double w = 2.0 * kPi * k / n;
    const double cw = std::cos(w);
    double       s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (Sample sample : signal) {
        s0 = static_cast<double>(sample) + 2.0 * cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double real = s1 - s2 * cw;
    const double imag = s2 * std::sin(w);
    return 2.0 * std::sqrt(real * real + imag * imag) / n;
}

double rms(const std::vector<Sample>& signal) {
    double sum = 0.0;
    for (Sample s : signal) {
        sum += static_cast<double>(s) * static_cast<double>(s);
    }
    return std::sqrt(sum / static_cast<double>(signal.size()));
}

// Full process()+drain() round trip for a single-channel signal.
std::vector<Sample> resampleMono(Resampler& resampler, const std::vector<Sample>& input, SampleRate targetRate,
                                  SampleRate sourceRate) {
    const auto expectedOut =
        static_cast<std::size_t>(static_cast<double>(input.size()) * static_cast<double>(targetRate) / static_cast<double>(sourceRate)) +
        256;
    std::vector<Sample> out(expectedOut);

    std::vector<std::span<const Sample>> planarIn{std::span<const Sample>(input)};
    std::vector<std::span<Sample>>       planarOut{std::span<Sample>(out)};

    const std::size_t produced =
        resampler.process(std::span<const std::span<const Sample>>(planarIn), input.size(),
                           std::span<std::span<Sample>>(planarOut), out.size());

    std::vector<Sample>            tail(256);
    std::vector<std::span<Sample>> tailOut{std::span<Sample>(tail)};
    const std::size_t              drained = resampler.drain(std::span<std::span<Sample>>(tailOut), tail.size());

    std::vector<Sample> result(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(produced));
    result.insert(result.end(), tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(drained));
    return result;
}

}  // namespace

TEST_CASE("Resampler is an exact passthrough when rates match", "[resampler]") {
    Resampler resampler(44100, 44100, 1, Resampler::Quality::Good);
    REQUIRE(resampler.isIdentity());

    auto input = makeSine(1000.0, 44100, 2000);
    auto out   = resampleMono(resampler, input, 44100, 44100);

    REQUIRE(out.size() == input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        REQUIRE(out[i] == input[i]);
    }
}

TEST_CASE("Resampler tracks sourceFramesConsumed monotonically", "[resampler]") {
    Resampler resampler(48000, 44100, 1, Resampler::Quality::Good);
    auto      input = makeSine(1000.0, 48000, 4000);

    std::vector<Sample>             out(4000);
    std::vector<std::span<const Sample>> planarIn{std::span<const Sample>(input)};
    std::vector<std::span<Sample>>       planarOut{std::span<Sample>(out)};

    resampler.process(std::span<const std::span<const Sample>>(planarIn), input.size(),
                       std::span<std::span<Sample>>(planarOut), out.size());

    // All 4000 source frames were fed; consumed count can lag by the kernel's lookahead but must
    // not exceed what was actually fed, and must be positive once enough input has landed.
    REQUIRE(resampler.sourceFramesConsumed() > 0);
    REQUIRE(resampler.sourceFramesConsumed() <= static_cast<aud::FrameIndex>(input.size()));
}

TEST_CASE("Resampler preserves a 1kHz tone with low THD+N across a rate conversion", "[resampler]") {
    constexpr SampleRate kSourceRate = 44100;
    constexpr SampleRate kTargetRate = 48000;
    constexpr double     kToneHz     = 1000.0;

    Resampler resampler(kSourceRate, kTargetRate, 1, Resampler::Quality::Good);
    auto      input = makeSine(kToneHz, kSourceRate, 44100);  // 1 second
    auto      out    = resampleMono(resampler, input, kTargetRate, kSourceRate);

    // Trim edge transients (resampler startup/drain latency) before analysis. The trimmed length
    // is then truncated to an exact integer number of tone periods at the target rate (48000/1000
    // = 48 samples/cycle here) — analysing a non-integer number of cycles with a single-bin
    // Goertzel/DFT causes spectral leakage (scalloping loss) that shows up as several dB of
    // *apparent* THD+N even for a mathematically perfect resample. That leakage artifact, not
    // resampler distortion, is what a naive trim-then-analyse would measure.
    REQUIRE(out.size() > 4000);
    std::vector<Sample> trimmed(out.begin() + 2000, out.end() - 2000);
    const auto           periodSamples = static_cast<std::size_t>(std::lround(static_cast<double>(kTargetRate) / kToneHz));
    const std::size_t    exactCycles   = trimmed.size() / periodSamples;
    std::vector<Sample>  steady(trimmed.begin(), trimmed.begin() + static_cast<std::ptrdiff_t>(exactCycles * periodSamples));

    const double fundamental = goertzelMagnitude(steady, kToneHz, kTargetRate);
    const double totalRms    = rms(steady);
    const double fundamentalRms = fundamental / std::sqrt(2.0);  // sine RMS from peak amplitude
    const double residual       = std::sqrt(std::max(0.0, totalRms * totalRms - fundamentalRms * fundamentalRms));

    REQUIRE(fundamentalRms > 0.5);  // the tone survived at close to unity amplitude
    const double thdN = 20.0 * std::log10(residual / fundamentalRms);
    REQUIRE(thdN < -60.0);
}

TEST_CASE("Resampler attenuates a tone above the target Nyquist (anti-aliasing)", "[resampler]") {
    constexpr SampleRate kSourceRate = 48000;
    constexpr SampleRate kTargetRate = 16000;  // Nyquist 8kHz
    constexpr double     kToneHz     = 10000.0;  // above target Nyquist, must be filtered out

    Resampler resampler(kSourceRate, kTargetRate, 1, Resampler::Quality::Good);
    auto      input = makeSine(kToneHz, kSourceRate, 48000);
    auto      out    = resampleMono(resampler, input, kTargetRate, kSourceRate);

    REQUIRE(out.size() > 4000);
    std::vector<Sample> steady(out.begin() + 1000, out.end() - 1000);
    const double        outputRms = rms(steady);

    REQUIRE(outputRms < 0.05);  // input had RMS ~0.707; anti-aliasing filter should remove it almost entirely
}

TEST_CASE("Resampler reset() clears history so a fresh tone starts cleanly", "[resampler]") {
    Resampler resampler(44100, 48000, 1, Resampler::Quality::Good);
    auto      input = makeSine(1000.0, 44100, 8000);
    (void)resampleMono(resampler, input, 48000, 44100);

    resampler.reset();
    REQUIRE(resampler.sourceFramesConsumed() == 0);

    auto out2 = resampleMono(resampler, input, 48000, 44100);
    REQUIRE(out2.size() > 4000);
}

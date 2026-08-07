#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "../../engine/analysis/loudness/k_weighting.hpp"

using Catch::Approx;
using aud::loudness::BiquadCoeffs;
using aud::loudness::highShelfCoeffs;
using aud::loudness::KWeightingFilter;
using aud::loudness::rlbHighPassCoeffs;

namespace {

constexpr double kPi = 3.14159265358979323846;

// |H(f)| for one biquad, evaluated directly from its transfer function (not by running samples
// through it) — an independent check on the coefficients themselves, decoupled from the Biquad
// runtime implementation under test elsewhere.
double biquadMagnitude(const BiquadCoeffs& c, double freqHz, double sampleRate) {
    const std::complex<double> z = std::exp(std::complex<double>(0.0, -2.0 * kPi * freqHz / sampleRate));
    const std::complex<double> num = c.b0 + c.b1 * z + c.b2 * (z * z);
    const std::complex<double> den = 1.0 + c.a1 * z + c.a2 * (z * z);
    return std::abs(num / den);
}

double kWeightingMagnitudeDb(double freqHz, double sampleRate) {
    const double shelf = biquadMagnitude(highShelfCoeffs(static_cast<aud::SampleRate>(sampleRate)), freqHz, sampleRate);
    const double rlb    = biquadMagnitude(rlbHighPassCoeffs(static_cast<aud::SampleRate>(sampleRate)), freqHz, sampleRate);
    return 20.0 * std::log10(shelf * rlb);
}

}  // namespace

TEST_CASE("k_weighting: recomputed 48 kHz coefficients match BS.1770's published table", "[loudness]") {
    // Published ITU-R BS.1770-4 Table 1 values. The analytic recomputation should reproduce these
    // to near machine precision since the published table is itself derived from this exact
    // analog prototype.
    const BiquadCoeffs shelf = highShelfCoeffs(48000);
    CHECK(shelf.b0 == Approx(1.53512485958697).margin(1e-3));
    CHECK(shelf.b1 == Approx(-2.69169618940638).margin(1e-3));
    CHECK(shelf.b2 == Approx(1.19839281085285).margin(1e-3));
    CHECK(shelf.a1 == Approx(-1.69065929318241).margin(1e-3));
    CHECK(shelf.a2 == Approx(0.73248077421585).margin(1e-3));

    const BiquadCoeffs rlb = rlbHighPassCoeffs(48000);
    CHECK(rlb.b0 == Approx(1.0).margin(1e-6));
    CHECK(rlb.b1 == Approx(-2.0).margin(1e-6));
    CHECK(rlb.b2 == Approx(1.0).margin(1e-6));
    CHECK(rlb.a1 == Approx(-1.99004745483398).margin(1e-3));
    CHECK(rlb.a2 == Approx(0.99007225036621).margin(1e-3));
}

TEST_CASE("k_weighting: frequency response is consistent across recomputed sample rates", "[loudness]") {
    // The whole point of M08's "recompute analytically, don't resample to 48 kHz" decision is
    // that evaluating the *same real-world frequency* at any sample rate should land on the same
    // point of the underlying analog prototype's curve. Cross-check 44.1/48/96/192 kHz against
    // each other directly (rather than against a hand-transcribed curve) — this is exactly the
    // risk M08's risk table calls out ("coefficient recomputation wrong at non-48kHz rates").
    const std::vector<double> rates = {44100.0, 48000.0, 96000.0, 192000.0};
    const std::vector<double> testFreqs = {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 15000.0};

    for (double freq : testFreqs) {
        const double reference = kWeightingMagnitudeDb(freq, 48000.0);
        for (double rate : rates) {
            if (rate == 48000.0) continue;
            const double got = kWeightingMagnitudeDb(freq, rate);
            CHECK(got == Approx(reference).margin(0.05));
        }
    }
}

TEST_CASE("k_weighting: response shape matches BS.1770's K-curve qualitatively", "[loudness]") {
    // Shelf boosts high frequencies (+4 dB approaching Nyquist-ish region above ~2kHz) and the
    // RLB stage rolls off strongly below ~40 Hz; check the well-known shape, not exact numbers.
    const double lowFreqDb  = kWeightingMagnitudeDb(20.0, 48000.0);
    const double midFreqDb  = kWeightingMagnitudeDb(1000.0, 48000.0);
    const double highFreqDb = kWeightingMagnitudeDb(10000.0, 48000.0);

    CHECK(lowFreqDb < midFreqDb - 10.0);   // strong high-pass roll-off at 20 Hz
    CHECK(highFreqDb > midFreqDb);         // shelf boost above the mid band
}

TEST_CASE("k_weighting: Biquad runtime output matches its own transfer function on a pure tone", "[loudness]") {
    // Sanity-check the *runtime* (transposed DF-II, double precision) against the same analytic
    // magnitude used above, by measuring the steady-state gain the filter actually applies to a
    // sine wave once transients have decayed.
    constexpr aud::SampleRate sampleRate = 48000;
    constexpr double          freq       = 1000.0;

    KWeightingFilter filter;
    filter.configure(sampleRate);

    constexpr int    n = 20000;
    std::vector<double> out(n);
    for (int i = 0; i < n; ++i) {
        const double x = std::sin(2.0 * kPi * freq * static_cast<double>(i) / sampleRate);
        out[i]          = filter.processSample(x);
    }

    double inPeak = 0.0, outPeak = 0.0;
    for (int i = n - 2000; i < n; ++i) {  // steady state only, skip the filter's settling transient
        outPeak = std::max(outPeak, std::abs(out[i]));
    }
    inPeak = 1.0;

    const double measuredDb  = 20.0 * std::log10(outPeak / inPeak);
    const double analyticDb  = kWeightingMagnitudeDb(freq, sampleRate);
    CHECK(measuredDb == Approx(analyticDb).margin(0.1));
}

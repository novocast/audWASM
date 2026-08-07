#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <vector>

#include "../../engine/analysis/beats/normalise.hpp"
#include "../../engine/analysis/beats/odf.hpp"
#include "../../engine/analysis/beats/whitening.hpp"

using Catch::Approx;
using aud::beats::NormaliseConfig;
using aud::beats::normaliseOdf;
using aud::beats::OdfComputer;
using aud::beats::OdfConfig;
using aud::beats::SpectralWhitener;
using aud::beats::WhiteningConfig;

namespace {
constexpr std::size_t kBinCount = 65;  // fftSize=128
}

TEST_CASE("beats::OdfComputer: an abrupt broadband jump produces a strong flux/hfc response, silence doesn't",
          "[beats][odf]") {
    OdfComputer computer(kBinCount, 44100, OdfConfig{});

    std::vector<float>               quiet(kBinCount, 0.0f);
    std::vector<std::complex<float>> quietComplex(kBinCount, std::complex<float>(0.0f, 0.0f));

    // Prime the "previous frame" state with quiet.
    [[maybe_unused]] const auto primed = computer.push(quiet, quietComplex);
    const auto stillQuiet = computer.push(quiet, quietComplex);
    CHECK(stillQuiet.flux == Approx(0.0).margin(1e-6));

    std::vector<float>               loud(kBinCount, 1.0f);
    std::vector<std::complex<float>> loudComplex(kBinCount);
    for (std::size_t k = 0; k < kBinCount; ++k) loudComplex[k] = std::complex<float>(1.0f, 0.0f);

    const auto jump = computer.push(loud, loudComplex);
    CHECK(jump.flux > 0.0f);
    CHECK(jump.hfc > 0.0f);
    CHECK(jump.complexDomain > 0.0f);
}

TEST_CASE("beats::OdfComputer: malformed input (wrong span length) returns a zeroed sample, not garbage",
          "[beats][odf]") {
    OdfComputer computer(kBinCount, 44100, OdfConfig{});
    std::vector<float>               tooShort(kBinCount - 1, 1.0f);
    std::vector<std::complex<float>> tooShortComplex(kBinCount - 1);

    const auto sample = computer.push(tooShort, tooShortComplex);
    CHECK(sample.flux == 0.0f);
    CHECK(sample.hfc == 0.0f);
    CHECK(sample.complexDomain == 0.0f);
}

TEST_CASE("beats::SpectralWhitener: a bin that has been loud stays whitened close to 1.0 while loud, "
          "and a newly loud bin isn't suppressed",
          "[beats][odf]") {
    SpectralWhitener whitener(4, WhiteningConfig{});

    std::vector<float> frame1 = {1.0f, 0.0f, 0.0f, 0.0f};
    whitener.apply(frame1);
    CHECK(frame1[0] == Approx(1.0).margin(1e-3));

    std::vector<float> frame2 = {1.0f, 0.0f, 0.0f, 0.0f};
    whitener.apply(frame2);
    // Bin 0 has been consistently loud — whitened value stays near 1.0, not suppressed toward 0.
    CHECK(frame2[0] == Approx(1.0).margin(1e-2));

    std::vector<float> frame3 = {0.0f, 1.0f, 0.0f, 0.0f};
    whitener.apply(frame3);
    // Bin 1 was previously silent (near-zero tracked peak) — its first loud frame is not divided
    // down by a stale large peak.
    CHECK(frame3[1] > 0.5f);
}

TEST_CASE("beats::normaliseOdf: a single spike among a flat baseline stands out after median/MAD normalisation",
          "[beats][odf]") {
    std::vector<float> raw(200, 0.1f);
    raw[100] = 5.0f;

    const auto normalised = normaliseOdf(raw, NormaliseConfig{50});

    CHECK(normalised[100] > 3.0f);
    CHECK(std::fabs(normalised[50]) < 1.0f);  // flat baseline stays near zero
}

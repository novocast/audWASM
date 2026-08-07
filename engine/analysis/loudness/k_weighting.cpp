#include "k_weighting.hpp"

#include <cmath>

namespace aud::loudness {

namespace {

// std::numbers::pi is C++20 but Emscripten's bundled libc++ lags (per M08's Result<T> comment on
// the same concern) — use a literal, same convention as the rest of the engine.
constexpr double kPi = 3.14159265358979323846;

}  // namespace

// Stage 1 — high-shelf ("head effect"), analytic fit to BS.1770's published 48 kHz coefficients
// (b0=1.53512485958697 b1=-2.69169618940638 b2=1.19839281085285 a1=-1.69065929318241
// a2=0.73248077421585). Derived via the Audio EQ Cookbook shelving-filter form with a corrective
// exponent (Vb) on the "boost at Nyquist" term needed to reproduce BS.1770's actual shelf shape
// exactly (a plain cookbook shelf does not) — this is the standard analytic K-weighting
// recomputation used by libebur128-class implementations.
BiquadCoeffs highShelfCoeffs(SampleRate sampleRate) noexcept {
    constexpr double kGainDb = 3.99984385397;
    constexpr double kQ      = 0.7071752369554193;
    constexpr double kFc     = 1681.9744509555319;
    constexpr double kVbExp  = 0.4996667741545416;

    const double fs = static_cast<double>(sampleRate);
    const double k  = std::tan(kPi * kFc / fs);
    const double vh = std::pow(10.0, kGainDb / 20.0);
    const double vb = std::pow(vh, kVbExp);

    const double a0Norm = 1.0 + k / kQ + k * k;

    BiquadCoeffs c;
    c.b0 = (vh + vb * k / kQ + k * k) / a0Norm;
    c.b1 = 2.0 * (k * k - vh) / a0Norm;
    c.b2 = (vh - vb * k / kQ + k * k) / a0Norm;
    c.a1 = 2.0 * (k * k - 1.0) / a0Norm;
    c.a2 = (1.0 - k / kQ + k * k) / a0Norm;
    return c;
}

// Stage 2 — RLB high-pass, analytic fit to BS.1770's published 48 kHz coefficients (b0=1.0
// b1=-2.0 b2=1.0 a1=-1.99004745483398 a2=0.99007225036621). Standard cookbook high-pass biquad,
// bilinear-transformed with pre-warping at the actual sample rate — except that, matching the
// published table (and libebur128/ffmpeg's ebur128 filter, which this must cross-validate against
// per M08's acceptance criteria), the numerator is left as the literal (1, -2, 1) rather than also
// being divided by a0Norm: only a1/a2 are normalized. Dividing b by a0Norm too gives a "more
// properly normalized" biquad in the abstract, but it disagrees with every compliant tool by the
// resulting ~0.04 dB at 48 kHz (a0Norm is very close to 1 since the RLB pole sits near DC) — this
// is a quirk of how the spec itself defines the filter, not a derivation error to correct.
BiquadCoeffs rlbHighPassCoeffs(SampleRate sampleRate) noexcept {
    constexpr double kQ  = 0.5003270373238773;
    constexpr double kFc = 38.13547087613982;

    const double fs = static_cast<double>(sampleRate);
    const double k  = std::tan(kPi * kFc / fs);

    const double a0Norm = 1.0 + k / kQ + k * k;

    BiquadCoeffs c;
    c.b0 = 1.0;
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (k * k - 1.0) / a0Norm;
    c.a2 = (1.0 - k / kQ + k * k) / a0Norm;
    return c;
}

}  // namespace aud::loudness

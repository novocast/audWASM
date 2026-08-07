#pragma once

// ITU-R BS.1770-4 K-weighting: a high-shelf ("head effect") stage cascaded with an RLB high-pass
// stage, both 2nd-order biquads. BS.1770 only publishes coefficients for 48 kHz; M08's decision is
// to recompute them analytically for the file's actual sample rate from the filter's analog
// pole/zero prototype (bilinear-transformed with frequency pre-warping), rather than resampling
// audio to 48 kHz first. The prototype parameters below (centre frequency, Q, shelf gain) are the
// standard fit to the published 48 kHz coefficients used by libebur128-class implementations —
// recomputing from them reproduces the published 48 kHz table to numerical precision and stays
// correct at every other rate. See k_weighting.cpp for the derivation and the frequency-response
// unit test for the numerical check at 44.1/48/96/192 kHz.
//
// Biquads run in double, transposed direct-form II, with denormal flushing: the RLB high-pass has
// poles very close to DC, and a long digital-silence passage would otherwise decay into denormal
// range and tank performance (a classic, very confusing slowdown — see M08's risk table).

#include <cstddef>
#include <span>
#include <vector>

#include "../../util/audio_types.hpp"

namespace aud::loudness {

// Coefficients for one biquad stage, in the standard b0,b1,b2 / a1,a2 form (a0 normalised to 1).
struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
};

// Recomputes the two K-weighting stages for `sampleRate`. Pure function of the prototype
// parameters; cheap enough to call once per Analyzer::begin() without a cache, but
// KWeightingCoefficientCache below keeps one anyway since several channels share the same rate.
[[nodiscard]] BiquadCoeffs highShelfCoeffs(SampleRate sampleRate) noexcept;
[[nodiscard]] BiquadCoeffs rlbHighPassCoeffs(SampleRate sampleRate) noexcept;

// A single transposed direct-form II biquad, run in double throughout.
class Biquad {
public:
    void configure(const BiquadCoeffs& coeffs) noexcept { m_coeffs = coeffs; }

    [[nodiscard]] double processSample(double x) noexcept {
        // Transposed DF-II: y = b0*x + s1; s1' = b1*x - a1*y + s2; s2' = b2*x - a2*y.
        const double y = m_coeffs.b0 * x + m_s1;
        m_s1           = m_coeffs.b1 * x - m_coeffs.a1 * y + m_s2;
        m_s2           = m_coeffs.b2 * x - m_coeffs.a2 * y;
        flushDenormals();
        return y;
    }

    void reset() noexcept { m_s1 = m_s2 = 0.0; }

private:
    // Cheap early flush well above true subnormal range (~1e-308): the RLB filter's near-DC pole
    // means state decays exponentially towards zero on silence and would otherwise spend a long
    // time transiting the (extremely slow-to-compute-on) denormal range before finally hitting
    // absolute zero. Flushing at 1e-30 is many orders of magnitude below any audible signal.
    void flushDenormals() noexcept {
        static constexpr double kThreshold = 1e-30;
        if (m_s1 > -kThreshold && m_s1 < kThreshold) m_s1 = 0.0;
        if (m_s2 > -kThreshold && m_s2 < kThreshold) m_s2 = 0.0;
    }

    BiquadCoeffs m_coeffs;
    double       m_s1 = 0.0;
    double       m_s2 = 0.0;
};

// The full K-weighting filter for one channel: high-shelf cascaded with RLB high-pass.
class KWeightingFilter {
public:
    void configure(SampleRate sampleRate) noexcept {
        m_shelf.configure(highShelfCoeffs(sampleRate));
        m_rlb.configure(rlbHighPassCoeffs(sampleRate));
    }

    [[nodiscard]] double processSample(double x) noexcept { return m_rlb.processSample(m_shelf.processSample(x)); }

    // Filters `in` into `out` (same length; may alias). Used per-channel per chunk.
    void process(std::span<const Sample> in, std::span<double> out) noexcept {
        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = processSample(static_cast<double>(in[i]));
        }
    }

    void reset() noexcept {
        m_shelf.reset();
        m_rlb.reset();
    }

private:
    Biquad m_shelf;
    Biquad m_rlb;
};

}  // namespace aud::loudness

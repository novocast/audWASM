#pragma once

// Analysis windows (M06 "Window functions"). Generated once into a table and applied as a
// multiply (see simd_kernels.hpp's windowMultiply()) — never recomputed per frame.
//
// Decision: default is Hann, periodic form. `w[n] = 0.5 * (1 - cos(2*pi*n/N))` with the N
// denominator, not N-1. The periodic form is correct for spectral analysis and overlap-add
// reconstruction; the symmetric (N-1) form is for FIR filter design. Getting this backwards
// introduces a small, hard-to-notice bias — see the M06 design doc — so both forms are provided,
// periodic is the default, and the parameter is always named explicitly at the call site.

#include <cstddef>
#include <span>

namespace aud::fft {

enum class WindowType {
    Rectangular,
    Hann,
    Hamming,
    Blackman,
    BlackmanHarris,  // 4-term
    Kaiser,
};

// Fills `out` (size out.size()) with the window's coefficients. `periodic = true` uses the
// N-denominator form; `periodic = false` uses the (N-1)-denominator form. `kaiserBeta` is ignored
// for every type except Kaiser.
void generateWindow(WindowType type, bool periodic, double kaiserBeta, std::span<float> out) noexcept;

// sum(w) / N — the coherent (DC) gain a full-scale sine's peak bin is scaled by. Needed for correct
// Amplitude/Power scaling (see scaling.hpp) — computing this ad hoc at the callsite is where
// scaling bugs come from (M06).
[[nodiscard]] double coherentGain(std::span<const float> window) noexcept;

// N * sum(w^2) / sum(w)^2 — the window's equivalent noise bandwidth in bins, needed for
// PowerDensity scaling (V^2/Hz for noise-like signals).
[[nodiscard]] double noisePowerBandwidth(std::span<const float> window) noexcept;

}  // namespace aud::fft

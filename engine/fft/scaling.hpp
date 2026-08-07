#pragma once

// Spectrum scaling conventions (M06 "Scaling — the thing everyone gets wrong"). Every consumer
// that turns FFT magnitudes into a displayable/measurable value goes through applyScaling() here —
// no ad-hoc normalisation at callsites, ever (see M06 acceptance criteria: "no consumer anywhere in
// the codebase computes its own ... scaling factor"). Magnitude/power extraction itself lives in
// simd_kernels.hpp, since that's the SIMD-relevant per-bin step; this file is only the
// normalisation-factor math applied on top of it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace aud::fft {

enum class SpectrumScaling {
    Raw,           // no normalisation; sum |X[k]|
    Amplitude,     // a full-scale sine at bin centre reads 1.0
    Power,         // amplitude^2
    PowerDensity,  // power / (fs * noisePowerBandwidth) — for noise-like signals, V^2/Hz
};

// Rescales an already-computed magnitude spectrum (see simd_kernels.hpp's magnitude()) in place,
// from raw unnormalised FFT units into `scaling`'s convention. `fftSize` is the transform size;
// `coherentGain`/`noisePowerBandwidth` come from the analysis window (windows.hpp); `sampleRate` is
// only used by PowerDensity. `magnitudes.size()` must be fftSize/2+1 (DC..Nyquist, one-sided).
//
// Decision (M06): the one-sided factor of 2 is applied to every bin except DC and (when fftSize is
// even) Nyquist — those two bins have no positive/negative-frequency pair to fold in. This is what
// makes a full-scale sine exactly at a bin centre read exactly 1.0 (0 dBFS) under Amplitude scaling
// — the first unit test written for this module.
inline void applyScaling(std::span<float> magnitudes, SpectrumScaling scaling, std::size_t fftSize,
                          double coherentGain, double noisePowerBandwidth, double sampleRate) noexcept {
    if (scaling == SpectrumScaling::Raw) {
        return;
    }

    const std::size_t nyquistBin    = fftSize / 2;
    const double      normalisation = static_cast<double>(fftSize) * coherentGain;

    for (std::size_t k = 0; k < magnitudes.size(); ++k) {
        const bool   isEdgeBin = (k == 0) || (fftSize % 2 == 0 && k == nyquistBin);
        const double factor    = isEdgeBin ? 1.0 : 2.0;
        const double amplitude = normalisation == 0.0
                                      ? 0.0
                                      : (static_cast<double>(magnitudes[k]) * factor) / normalisation;

        double value = amplitude;
        if (scaling == SpectrumScaling::Power || scaling == SpectrumScaling::PowerDensity) {
            value = amplitude * amplitude;
        }
        if (scaling == SpectrumScaling::PowerDensity) {
            const double denom = sampleRate * noisePowerBandwidth;
            value              = denom == 0.0 ? 0.0 : value / denom;
        }
        magnitudes[k] = static_cast<float>(value);
    }
}

// 20*log10(max(amplitude, floor)) — never log10(0). `floorLinear` defaults to 1e-12 per M06.
[[nodiscard]] inline double toDb(double amplitude, double floorLinear = 1e-12) noexcept {
    return 20.0 * std::log10(std::max(amplitude, floorLinear));
}

}  // namespace aud::fft

#pragma once

// Quadratic peak interpolation across 3 bins of log-magnitude (M06 "Reassignment & interpolation
// (scoped, not built)"). ~10x frequency-accuracy improvement over reading the raw bin, for the cost
// of a handful of scalar ops — worth having in v1 as a small utility. Full time-frequency
// reassignment (~3x the FFT cost) is deliberately deferred; StftFrame's layout (see stft.hpp) is
// kept plain for exactly that reason, so reassignment offsets can be bolted on later without
// breaking it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace aud::fft {

struct PeakEstimate {
    double binOffset    = 0.0;  // fractional offset from `bin`, in [-0.5, 0.5]
    double logMagnitude = 0.0;  // interpolated peak log-magnitude (natural log)
};

// `bin` must be a local maximum in `magnitudes` and strictly between its first and last index —
// edge bins have no interpolation neighbour on one side and must be handled by the caller before
// calling this.
[[nodiscard]] inline PeakEstimate interpolatePeak(std::span<const float> magnitudes, std::size_t bin) noexcept {
    constexpr double kFloor = 1e-12;
    const double     left   = std::log(std::max(static_cast<double>(magnitudes[bin - 1]), kFloor));
    const double     center = std::log(std::max(static_cast<double>(magnitudes[bin]), kFloor));
    const double     right  = std::log(std::max(static_cast<double>(magnitudes[bin + 1]), kFloor));

    const double denom = left - 2.0 * center + right;
    if (denom == 0.0) {
        return PeakEstimate{0.0, center};
    }
    const double offset = 0.5 * (left - right) / denom;
    const double peak    = center - 0.25 * (left - right) * offset;
    return PeakEstimate{offset, peak};
}

}  // namespace aud::fft

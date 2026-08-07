#include "simd_kernels.hpp"

#include <cmath>

namespace aud::fft {

void windowMultiply(std::span<const float> samples, std::span<const float> window,
                     std::span<float> out) noexcept {
    const std::size_t n = samples.size();
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = samples[i] * window[i];
    }
}

void magnitude(std::span<const std::complex<float>> bins, std::span<float> out) noexcept {
    const std::size_t n = bins.size();
    for (std::size_t i = 0; i < n; ++i) {
        const float re = bins[i].real();
        const float im = bins[i].imag();
        out[i]         = std::sqrt(re * re + im * im);
    }
}

void power(std::span<const std::complex<float>> bins, std::span<float> out) noexcept {
    const std::size_t n = bins.size();
    for (std::size_t i = 0; i < n; ++i) {
        const float re = bins[i].real();
        const float im = bins[i].imag();
        out[i]         = re * re + im * im;
    }
}

}  // namespace aud::fft

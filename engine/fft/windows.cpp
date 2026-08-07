#include "windows.hpp"

#include <cmath>

namespace aud::fft {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Modified Bessel function of the first kind, order 0 — needed for the Kaiser window. Series
// expansion converges quickly (the loop bails once a term stops contributing) for the beta range
// (0-20ish) Kaiser windows are actually used with.
double besselI0(double x) noexcept {
    double sum       = 1.0;
    double term      = 1.0;
    const double half = x / 2.0;
    for (int k = 1; k < 64; ++k) {
        term *= (half * half) / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-16) {
            break;
        }
    }
    return sum;
}

}  // namespace

void generateWindow(WindowType type, bool periodic, double kaiserBeta, std::span<float> out) noexcept {
    const std::size_t n = out.size();
    if (n == 0) {
        return;
    }
    // Periodic uses N as the denominator (correct for spectral analysis/OLA); symmetric uses N-1
    // (correct for FIR filter design) — see the M06 design doc's "the thing everyone gets wrong".
    const double denom = periodic ? static_cast<double>(n) : static_cast<double>(n > 1 ? n - 1 : 1);

    switch (type) {
        case WindowType::Rectangular:
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = 1.0f;
            }
            break;

        case WindowType::Hann:
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / denom)));
            }
            break;

        case WindowType::Hamming:
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = static_cast<float>(0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(i) / denom));
            }
            break;

        case WindowType::Blackman:
            for (std::size_t i = 0; i < n; ++i) {
                const double phase = 2.0 * kPi * static_cast<double>(i) / denom;
                out[i] = static_cast<float>(0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase));
            }
            break;

        case WindowType::BlackmanHarris:
            for (std::size_t i = 0; i < n; ++i) {
                const double phase = 2.0 * kPi * static_cast<double>(i) / denom;
                out[i]             = static_cast<float>(0.35875 - 0.48829 * std::cos(phase) +
                                              0.14128 * std::cos(2.0 * phase) - 0.01168 * std::cos(3.0 * phase));
            }
            break;

        case WindowType::Kaiser: {
            const double i0Beta = besselI0(kaiserBeta);
            for (std::size_t i = 0; i < n; ++i) {
                const double x   = (2.0 * static_cast<double>(i) - denom) / denom;
                const double arg = 1.0 - x * x;
                out[i]           = static_cast<float>(besselI0(kaiserBeta * std::sqrt(arg < 0.0 ? 0.0 : arg)) / i0Beta);
            }
            break;
        }
    }
}

double coherentGain(std::span<const float> window) noexcept {
    if (window.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (float w : window) {
        sum += static_cast<double>(w);
    }
    return sum / static_cast<double>(window.size());
}

double noisePowerBandwidth(std::span<const float> window) noexcept {
    if (window.empty()) {
        return 0.0;
    }
    double sum    = 0.0;
    double sumSq  = 0.0;
    for (float w : window) {
        sum += static_cast<double>(w);
        sumSq += static_cast<double>(w) * static_cast<double>(w);
    }
    if (sum == 0.0) {
        return 0.0;
    }
    return static_cast<double>(window.size()) * sumSq / (sum * sum);
}

}  // namespace aud::fft

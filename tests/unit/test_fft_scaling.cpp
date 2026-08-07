#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "../../engine/fft/real_fft.hpp"
#include "../../engine/fft/scaling.hpp"
#include "../../engine/fft/simd_kernels.hpp"
#include "../../engine/fft/windows.hpp"

using aud::fft::applyScaling;
using aud::fft::coherentGain;
using aud::fft::generateWindow;
using aud::fft::magnitude;
using aud::fft::noisePowerBandwidth;
using aud::fft::RealFft;
using aud::fft::SpectrumScaling;
using aud::fft::toDb;
using aud::fft::windowMultiply;
using aud::fft::WindowType;
using Catch::Approx;

namespace {
constexpr double kPi = 3.14159265358979323846;
}

TEST_CASE("scaling: toDb never takes log10(0)", "[fft][scaling]") {
    REQUIRE(std::isfinite(toDb(0.0)));
    REQUIRE(toDb(0.0) == Approx(-240.0).margin(1.0));
}

TEST_CASE("scaling: full-scale sine at an exact bin centre reads 0 dBFS under Amplitude scaling",
          "[fft][scaling]") {
    constexpr std::size_t n  = 2048;
    constexpr std::size_t k0 = n / 8;  // far from DC/Nyquist and from every window's cosine support

    const auto windows = {WindowType::Rectangular, WindowType::Hann,           WindowType::Hamming,
                           WindowType::Blackman,    WindowType::BlackmanHarris, WindowType::Kaiser};

    for (auto windowType : windows) {
        CAPTURE(static_cast<int>(windowType));

        std::vector<float> window(n);
        generateWindow(windowType, /*periodic=*/true, /*kaiserBeta=*/8.6, window);
        const double gain = coherentGain(window);
        const double npb  = noisePowerBandwidth(window);

        std::vector<float> signal(n);
        for (std::size_t i = 0; i < n; ++i) {
            signal[i] = static_cast<float>(
                std::cos(2.0 * kPi * static_cast<double>(k0) * static_cast<double>(i) / static_cast<double>(n)));
        }

        std::vector<float> windowed(n);
        windowMultiply(signal, window, windowed);

        auto created = RealFft::create(n);
        REQUIRE(created.has_value());
        std::vector<std::complex<float>> spectrum(n / 2 + 1);
        created.value()->forward(windowed, spectrum);

        std::vector<float> mags(n / 2 + 1);
        magnitude(spectrum, mags);
        applyScaling(mags, SpectrumScaling::Amplitude, n, gain, npb, 48000.0);

        const double db = toDb(mags[k0]);
        REQUIRE(db == Approx(0.0).margin(0.01));
    }
}

TEST_CASE("scaling: DC and Nyquist bins never get the one-sided factor of 2", "[fft][scaling]") {
    constexpr std::size_t n = 256;
    std::vector<float>    mags(n / 2 + 1, 1.0f);
    applyScaling(mags, SpectrumScaling::Raw, n, 1.0, 1.0, 48000.0);
    // Raw is a no-op; re-run with Amplitude and check the edge/interior ratio is exactly 2.
    std::vector<float> amp(n / 2 + 1, 1.0f);
    applyScaling(amp, SpectrumScaling::Amplitude, n, 1.0, 1.0, 48000.0);
    REQUIRE(amp[1] == Approx(2.0 * amp[0]));
    REQUIRE(amp[n / 2] == Approx(amp[0]));
}

TEST_CASE("scaling: Power is Amplitude squared", "[fft][scaling]") {
    constexpr std::size_t n = 256;
    std::vector<float>    amp(n / 2 + 1, 1.0f);
    std::vector<float>    power(n / 2 + 1, 1.0f);
    applyScaling(amp, SpectrumScaling::Amplitude, n, 1.0, 1.0, 48000.0);
    applyScaling(power, SpectrumScaling::Power, n, 1.0, 1.0, 48000.0);
    for (std::size_t k = 0; k < amp.size(); ++k) {
        REQUIRE(power[k] == Approx(static_cast<double>(amp[k]) * amp[k]).margin(1e-9));
    }
}

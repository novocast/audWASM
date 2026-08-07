#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "../../engine/fft/windows.hpp"

using aud::fft::coherentGain;
using aud::fft::generateWindow;
using aud::fft::noisePowerBandwidth;
using aud::fft::WindowType;
using Catch::Approx;

TEST_CASE("windows: rectangular window has unity coherent gain and NPB", "[fft][windows]") {
    std::vector<float> window(256);
    generateWindow(WindowType::Rectangular, /*periodic=*/true, /*kaiserBeta=*/0.0, window);
    REQUIRE(coherentGain(window) == Approx(1.0));
    REQUIRE(noisePowerBandwidth(window) == Approx(1.0));
}

TEST_CASE("windows: Hann coherent gain is 0.5", "[fft][windows]") {
    std::vector<float> window(1024);
    generateWindow(WindowType::Hann, /*periodic=*/true, /*kaiserBeta=*/0.0, window);
    REQUIRE(coherentGain(window) == Approx(0.5).margin(1e-6));
}

TEST_CASE("windows: Hann at 50% overlap satisfies COLA", "[fft][windows]") {
    constexpr std::size_t n   = 1024;
    constexpr std::size_t hop = n / 2;

    std::vector<float> window(n);
    generateWindow(WindowType::Hann, /*periodic=*/true, /*kaiserBeta=*/0.0, window);

    constexpr std::size_t   totalLength = n * 4;
    std::vector<double>     sum(totalLength, 0.0);
    for (std::size_t start = 0; start + n <= totalLength; start += hop) {
        for (std::size_t i = 0; i < n; ++i) {
            sum[start + i] += window[i];
        }
    }

    // Skip the first/last window's length: the overlap-add ramps up/down there and only the
    // interior is guaranteed to have reached the steady-state constant.
    const double reference = sum[n];
    for (std::size_t i = n; i < totalLength - n; ++i) {
        REQUIRE(sum[i] == Approx(reference).margin(1e-5));
    }
}

TEST_CASE("windows: periodic vs symmetric form differ only at the denominator", "[fft][windows]") {
    constexpr std::size_t n = 8;
    std::vector<float>    periodic(n);
    std::vector<float>    symmetric(n);
    generateWindow(WindowType::Hann, /*periodic=*/true, /*kaiserBeta=*/0.0, periodic);
    generateWindow(WindowType::Hann, /*periodic=*/false, /*kaiserBeta=*/0.0, symmetric);

    // Symmetric Hann is exactly mirror-symmetric about its centre; periodic Hann is not (its last
    // sample equals what would be one step past the symmetric window's last sample).
    for (std::size_t i = 0; i < n; ++i) {
        REQUIRE(symmetric[i] == Approx(symmetric[n - 1 - i]).margin(1e-6));
    }
    REQUIRE(periodic[0] == Approx(0.0).margin(1e-6));
}

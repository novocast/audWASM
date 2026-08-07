#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

#include "../../engine/fft/naive_dft.hpp"
#include "../../engine/fft/real_fft.hpp"

using aud::fft::RealFft;
using aud::fft::testing::createNaiveDft;
using Catch::Approx;

namespace {

std::vector<float> randomSignal(std::size_t n, std::uint32_t seed) {
    std::mt19937                          rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float>                    signal(n);
    for (auto& s : signal) {
        s = dist(rng);
    }
    return signal;
}

}  // namespace

TEST_CASE("RealFft: PocketFFT matches the naive O(N^2) reference DFT", "[fft]") {
    for (std::size_t n : {std::size_t{16}, std::size_t{64}, std::size_t{256}, std::size_t{1024},
                           std::size_t{2048}, std::size_t{4096}}) {
        CAPTURE(n);
        auto pocket = RealFft::create(n);
        REQUIRE(pocket.has_value());
        auto naive = createNaiveDft(n);

        auto signal = randomSignal(n, static_cast<std::uint32_t>(n));

        std::vector<std::complex<float>> pocketOut(n / 2 + 1);
        std::vector<std::complex<float>> naiveOut(n / 2 + 1);
        pocket.value()->forward(signal, pocketOut);
        naive->forward(signal, naiveOut);

        double maxRelError = 0.0;
        for (std::size_t k = 0; k < pocketOut.size(); ++k) {
            const double refMag    = std::abs(naiveOut[k]);
            const double diff      = std::abs(pocketOut[k] - naiveOut[k]);
            const double relError  = diff / std::max(refMag, 1e-6);
            maxRelError            = std::max(maxRelError, relError);
        }
        REQUIRE(maxRelError < 1e-5);
    }
}

TEST_CASE("RealFft: inverse round-trips forward within float precision", "[fft]") {
    for (std::size_t n : {std::size_t{16}, std::size_t{64}, std::size_t{256}, std::size_t{1024},
                           std::size_t{2048}}) {
        CAPTURE(n);
        auto created = RealFft::create(n);
        REQUIRE(created.has_value());
        auto& transform = *created.value();

        auto signal = randomSignal(n, static_cast<std::uint32_t>(n) * 7 + 1);
        std::vector<std::complex<float>> spectrum(n / 2 + 1);
        std::vector<float>               reconstructed(n);

        transform.forward(signal, spectrum);
        transform.inverse(spectrum, reconstructed);

        double maxAbsError = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            maxAbsError = std::max(maxAbsError, static_cast<double>(std::abs(signal[i] - reconstructed[i])));
        }
        REQUIRE(maxAbsError < 1e-4);
    }
}

TEST_CASE("RealFft: Parseval's theorem holds", "[fft]") {
    for (std::size_t n : {std::size_t{64}, std::size_t{256}, std::size_t{1024}}) {
        CAPTURE(n);
        auto created = RealFft::create(n);
        REQUIRE(created.has_value());
        auto& transform = *created.value();

        auto signal = randomSignal(n, static_cast<std::uint32_t>(n) * 13 + 3);
        std::vector<std::complex<float>> spectrum(n / 2 + 1);
        transform.forward(signal, spectrum);

        double timeEnergy = 0.0;
        for (float s : signal) {
            timeEnergy += static_cast<double>(s) * s;
        }

        double     freqEnergy = 0.0;
        const bool even       = (n % 2 == 0);
        for (std::size_t k = 0; k < spectrum.size(); ++k) {
            const double mag2   = std::norm(spectrum[k]);
            const bool   isEdge = (k == 0) || (even && k == n / 2);
            freqEnergy += isEdge ? mag2 : 2.0 * mag2;
        }
        freqEnergy /= static_cast<double>(n);

        const double relError = std::abs(timeEnergy - freqEnergy) / std::max(timeEnergy, 1e-9);
        REQUIRE(relError < 1e-5);
    }
}

TEST_CASE("RealFft: create() rejects unsupported sizes", "[fft]") {
    REQUIRE_FALSE(RealFft::create(0).has_value());
    REQUIRE_FALSE(RealFft::create(1).has_value());
    // 8191 is prime and > 11, so its largest prime factor is itself.
    REQUIRE_FALSE(RealFft::create(8191).has_value());
    REQUIRE(RealFft::create(2048).has_value());
}

TEST_CASE("RealFft: 2048-point forward FFT throughput", "[fft][.benchmark]") {
    auto created = RealFft::create(2048);
    REQUIRE(created.has_value());
    auto& transform = *created.value();
    auto  signal     = randomSignal(2048, 42);
    std::vector<std::complex<float>> spectrum(2048 / 2 + 1);

    BENCHMARK("2048-point real forward FFT") {
        transform.forward(signal, spectrum);
        return spectrum[1];
    };
}

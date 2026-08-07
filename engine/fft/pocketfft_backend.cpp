// PocketFFT (BSD-3, vendored header-only, see third_party/VERSIONS.md) backend for
// aud::fft::RealFft. Chosen over KissFFT (slower/less accurate for large transforms) and FFTW
// (GPL/commercial, doesn't sensibly target WASM) — see M06's backend decision table. Only `float`
// is instantiated, deliberately, to keep the WASM binary from paying for template instantiations
// (double, long double) this codebase never needs (M06 risk: "PocketFFT's C++ header inflating
// WASM size").

#include "real_fft.hpp"

#include <memory>

#include "../../third_party/pocketfft_hdronly.h"

namespace aud::fft {

namespace {

std::size_t largestPrimeFactor(std::size_t n) noexcept {
    std::size_t largest   = 1;
    std::size_t remaining = n;
    for (std::size_t factor = 2; factor * factor <= remaining; ++factor) {
        while (remaining % factor == 0) {
            largest = factor;
            remaining /= factor;
        }
    }
    if (remaining > 1) {
        largest = remaining;
    }
    return largest;
}

class PocketFftRealFft final : public RealFft {
public:
    explicit PocketFftRealFft(std::size_t n) : m_n(n) {}

    [[nodiscard]] std::size_t size() const noexcept override { return m_n; }

    void forward(std::span<const float> in, std::span<std::complex<float>> out) override {
        const pocketfft::shape_t  shape{m_n};
        const pocketfft::stride_t strideIn{sizeof(float)};
        const pocketfft::stride_t strideOut{sizeof(std::complex<float>)};
        // forward=true is PocketFFT's "traditional" r2c direction (minus sign in the exponent,
        // matching numpy/scipy rfft) — see the upstream README's `forward` argument note.
        pocketfft::r2c(shape, strideIn, strideOut, /*axis=*/0, /*forward=*/true, in.data(), out.data(),
                       /*fct=*/1.0f);
    }

    void inverse(std::span<const std::complex<float>> in, std::span<float> out) override {
        const pocketfft::shape_t  shape{m_n};
        const pocketfft::stride_t strideIn{sizeof(std::complex<float>)};
        const pocketfft::stride_t strideOut{sizeof(float)};
        // PocketFFT never normalises its own transforms (see README's `fct` note); forward=false
        // pairs with r2c's forward=true to give the traditional inverse, and dividing by n here is
        // what makes inverse(forward(x)) round-trip to x.
        const float fct = 1.0f / static_cast<float>(m_n);
        pocketfft::c2r(shape, strideIn, strideOut, /*axis=*/0, /*forward=*/false, in.data(), out.data(), fct);
    }

private:
    std::size_t m_n;
};

}  // namespace

bool isSupportedFftSize(std::size_t n) noexcept {
    return n >= 2 && largestPrimeFactor(n) <= 11;
}

Result<std::unique_ptr<RealFft>> RealFft::create(std::size_t n) {
    if (!isSupportedFftSize(n)) {
        return Error{ErrorCode::InvalidArgument, "fft.real_fft",
                     "FFT size must be >= 2 with largest prime factor <= 11"};
    }
    return std::unique_ptr<RealFft>(std::make_unique<PocketFftRealFft>(n));
}

}  // namespace aud::fft

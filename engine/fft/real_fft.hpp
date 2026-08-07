#pragma once

// Backend abstraction for real-input FFTs (M06). One implementation exists today (PocketFFT, see
// pocketfft_backend.cpp) but the seam stays: a hand-tuned WASM SIMD kernel or FFTW/MKL on native
// could be dropped in behind create() without touching any consumer. Plans are created once and
// reused — StftProcessor owns one for its lifetime rather than creating one per frame, since plan
// setup (twiddle factors) would dominate an STFT loop's runtime otherwise.

#include <complex>
#include <cstddef>
#include <memory>
#include <span>

#include "../util/result.hpp"

namespace aud::fft {

// True if `n` is a size RealFft::create() will accept: >= 2, with largest prime factor <= 11.
// PocketFFT's efficient real-FFT codelets cover factors 2/3/4/5; the generic path handles larger
// prime factors correctly but slowly (M06 risk: "non-power-of-two sizes silently taking a slow
// path"). Restricting the public API to this set catches an unreasonable size at construction
// rather than letting it silently degrade at runtime.
[[nodiscard]] bool isSupportedFftSize(std::size_t n) noexcept;

// Real input -> N/2+1 complex bins (DC ... Nyquist inclusive).
class RealFft {
public:
    static Result<std::unique_ptr<RealFft>> create(std::size_t n);

    virtual ~RealFft() = default;

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;

    // in.size() must be size(); out.size() must be size()/2+1.
    virtual void forward(std::span<const float> in, std::span<std::complex<float>> out) = 0;

    // in.size() must be size()/2+1; out.size() must be size(). Normalised so that
    // inverse(forward(x)) round-trips to x within float precision.
    virtual void inverse(std::span<const std::complex<float>> in, std::span<float> out) = 0;
};

}  // namespace aud::fft

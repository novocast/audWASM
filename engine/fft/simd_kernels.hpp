#pragma once

// Window-multiply / magnitude / power kernels (M06 "SIMD & performance"). Each is elementwise —
// output[i] depends on nothing but input[i] — so -O3 plus the project-wide -mavx2 (native) /
// -msimd128 (WASM) flags (see aud_project_options in the root CMakeLists) auto-vectorise these
// without hand intrinsics. This codebase has no hand-vectorised code anywhere else; introducing
// three separate intrinsic sets (AVX2/NEON/wasm SIMD) for kernels this simple isn't worth the
// maintenance cost unless profiling later shows the compiler's autovectorisation falls short.
//
// Because there's no cross-lane reduction, the vectorised and scalar forms compute the exact same
// per-element result — the `*Scalar` names are the M06 task list's "scalar reference": an explicit,
// obviously-correct entry point tests can compare the production path against.

#include <complex>
#include <cstddef>
#include <span>

namespace aud::fft {

// out[i] = samples[i] * window[i]. samples/window/out must all be the same size.
void windowMultiply(std::span<const float> samples, std::span<const float> window,
                     std::span<float> out) noexcept;
inline void windowMultiplyScalar(std::span<const float> samples, std::span<const float> window,
                                  std::span<float> out) noexcept {
    windowMultiply(samples, window, out);
}

// out[i] = sqrt(re^2 + im^2). Prefer power() below when only relative magnitude/thresholding is
// needed (onset detection etc.) — it skips the sqrt.
void magnitude(std::span<const std::complex<float>> bins, std::span<float> out) noexcept;
inline void magnitudeScalar(std::span<const std::complex<float>> bins, std::span<float> out) noexcept {
    magnitude(bins, out);
}

// out[i] = re^2 + im^2.
void power(std::span<const std::complex<float>> bins, std::span<float> out) noexcept;
inline void powerScalar(std::span<const std::complex<float>> bins, std::span<float> out) noexcept {
    power(bins, out);
}

}  // namespace aud::fft

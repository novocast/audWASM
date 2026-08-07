#pragma once

// O(N^2) reference real DFT (M06 task list). Exists solely so tests can cross-check
// PocketFftRealFft's output against an obviously-correct, independently-implemented transform —
// never linked into aud_core, never used outside tests/.

#include <memory>

#include "real_fft.hpp"

namespace aud::fft::testing {

[[nodiscard]] std::unique_ptr<RealFft> createNaiveDft(std::size_t n);

}  // namespace aud::fft::testing

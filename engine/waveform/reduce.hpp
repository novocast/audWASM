#pragma once

// Scalar reference reduction (M04 "Reduction algorithm"). This is the ground truth a future SIMD
// path (v128/AVX2/NEON, tracked as remaining M04 work) must be validated bit-comparably against;
// keep it simple and obviously correct rather than fast.

#include <span>
#include <vector>

#include "../util/audio_types.hpp"
#include "waveform_bin.hpp"

namespace aud::waveform {

// Reduces exactly one bin covering `samples` (any length, including empty). An empty span yields
// a zeroed bin, matching the digital-silence contract.
[[nodiscard]] WaveformBin reduceOneBin(std::span<const Sample> samples) noexcept;

// Splits `samples` into consecutive bins of `binFrames` each (the final bin may be shorter, never
// spanning past samples.size()) and appends the reduced result of each to `out`. No state is kept
// across calls — callers rely on that to reduce chunks independently (M04 streaming generation).
void reduceToBins(std::span<const Sample> samples, std::size_t binFrames, std::vector<WaveformBin>& out);

}  // namespace aud::waveform

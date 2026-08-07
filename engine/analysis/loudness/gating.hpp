#pragma once

// ITU-R BS.1770-4 two-stage integrated-loudness gate. Operates on *linear* channel-weighted mean
// squares (the same values BlockAccumulator emits per 400 ms momentary block, pre-log) — not on
// dB loudness values directly, because the gate's own "mean loudness" step must average in the
// power domain and only convert to LU once, per spec. This is the step M08's design doc calls out
// as "the step most naive implementations skip or get wrong": averaging the block loudness *values*
// (already in dB) instead of their underlying power would silently disagree with every compliant
// tool.

#include <cstddef>
#include <span>

namespace aud::loudness {

// -0.691 + 10*log10(z), or -infinity for z <= 0 (silence). Shared by gating, LRA and the
// momentary/short-term series so the exact same formula is used everywhere per BS.1770 Eq. 2.
[[nodiscard]] double loudnessFromMeanSquare(double weightedMeanSquare) noexcept;

// Inverse of loudnessFromMeanSquare — the linear mean square that maps to a given LUFS/LU value.
[[nodiscard]] double meanSquareFromLoudness(double lufs) noexcept;

struct GateResult {
    double integratedLufs;  // NaN if no block survives either gate (e.g. pure silence)
};

// `momentaryMeanSquares` is one entry per completed 400 ms momentary block (75% overlap, i.e.
// every 100 ms sub-block once warmed up) across the whole programme — exactly the accumulator
// BlockAccumulator + a 4-wide SlidingWindowSum produces. Applies the absolute gate (-70 LUFS) then
// the relative gate (mean of survivors - 10 LU) and returns the integrated loudness from what
// remains.
[[nodiscard]] GateResult gateIntegratedLoudness(std::span<const double> momentaryMeanSquares);

}  // namespace aud::loudness

#pragma once

// TT-style "DR" measure (M09 "DR (TT-style)") — the loudness-war community's convention, distinct
// from crest factor and from LRA (M08). Deliberately labelled a *convention*, not a standard, in
// every surface that reports it.

#include <vector>

namespace aud::statistics {

// `blockRms`/`blockPeaks` are per-3-second-block RMS and peak (linear, not dB) — ChannelAccumulator
// derives these from its 50ms windowed RMS series (M09: "derive 3s blocks from those"). Per the
// convention: take the top 20% of blocks by RMS, DR = 20*log10(mean(peak) / rms) over that subset,
// where `rms` there is the RMS-of-the-RMS-values (power-domain average) of the same subset.
// Returns 0.0 if fewer than one qualifying block exists (too short to measure).
[[nodiscard]] double computeDynamicRangeDb(const std::vector<double>& blockRms, const std::vector<double>& blockPeaks);

}  // namespace aud::statistics

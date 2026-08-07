#pragma once

// EBU Tech 3342 Loudness Range (LRA): its own two-stage gate (absolute -70 LUFS, relative -20 LU
// — not the integrated measurement's -10 LU) applied to short-term loudness values sampled every
// 1 s over a 3 s window (note: a different hop from the meter's 100 ms-resolution short-term
// series computed elsewhere in the pipeline), then LRA = P95 - P10 of what survives. The
// asymmetric percentile range (not a symmetric spread) and the distinct gating are the two things
// M08's design doc calls out as easy to get wrong.

#include <cstddef>
#include <span>

namespace aud::loudness {

// `shortTermMeanSquares` is one linear channel-weighted mean square per 3 s/1 s-hop short-term
// block (i.e. NOT the 100 ms-resolution meter series — see loudness_analyzer.cpp for how the two
// are kept separate from the same underlying sub-block stream). Returns the LRA in LU, or NaN if
// nothing survives the absolute gate (mirrors the integrated measurement's silence behaviour).
[[nodiscard]] double computeLoudnessRange(std::span<const double> shortTermMeanSquares);

}  // namespace aud::loudness

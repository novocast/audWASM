#pragma once

// M13's ODF normalisation: subtract a moving median and divide by a moving MAD over ~1s (the
// doc's "Adaptive whitening and normalisation" §2). Median/MAD rather than mean/stddev because
// the ODF is spiky by construction — a mean would be dragged upward by the very peaks peak
// picking needs to see clearly above the noise floor.
//
// This is a pure function over an already-computed ODF series (not a streaming Analyzer stage):
// the acceptance criterion "threshold changes re-pick peaks instantly without recomputing the
// STFT" only holds if re-normalising and re-picking are cheap, which requires the raw ODF to be
// retained and this to be callable standalone — see BeatResult::odf.

#include <cstddef>
#include <span>
#include <vector>

namespace aud::beats {

struct NormaliseConfig {
    std::size_t windowFrames = 86;  // ~1s at fftSize=2048/hop=512/44.1kHz (doc: "moving ... over ~1s")
};

// Returns a new series the same length as `raw`: `(raw[i] - median(window)) / (1.4826*mad(window) + eps)`,
// where `window` is `windowFrames` samples centred on `i` (clamped at the array's edges). Centred
// rather than causal — this runs once over the whole retained ODF at finish(), not per-frame in a
// live stream, and a causal window measurably biases onset timing on periodic material (see
// normalise.cpp).
[[nodiscard]] std::vector<float> normaliseOdf(std::span<const float> raw, NormaliseConfig config = {});

}  // namespace aud::beats

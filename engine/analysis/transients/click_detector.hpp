#pragma once

// M14's click/pop detector — see documentation/tasks/M14-transient-detection.md "Defect detection"
// / "Clicks / pops". LPC residual analysis, the standard approach declickers use, and far more
// reliable than a raw difference threshold (which false-positives on every drum hit):
//
//   1. Fit a short linear predictor (order ~8-16) over a sliding window of raw PCM.
//   2. Where the prediction residual spikes far above its local median (>~6x MAD), flag a
//      candidate — the waveform made a jump the surrounding bandwidth cannot explain.
//   3. Reject candidates that coincide with a detected musical onset (rejectOnsetCoincidences) — a
//      snare hit is also unpredictable to a short-order LPC, and this is the doc's explicit
//      zero-false-positive-on-percussion criterion.
//
// Runs on the same whole-track mono PCM as refine_timing.hpp (see transient_analyzer.hpp's header
// comment for why this module keeps full-track PCM rather than streaming).

#include <cstddef>
#include <span>
#include <vector>

#include "../../util/audio_types.hpp"

namespace aud::transients {

struct ClickDetectorConfig {
    std::size_t lpcOrder      = 12;
    std::size_t windowSamples = 512;   // LPC fit window
    std::size_t hopSamples    = 256;   // advance per iteration; must be <= windowSamples
    double      madMultiplier = 6.0;   // doc: "say >6x MAD"
    double      minAbsResidual = 1e-4;  // floor so near-digital-silence doesn't produce spurious relative spikes
};

struct ClickCandidate {
    FrameIndex frame         = kNoFrame;  // sample of the residual spike
    float      residualRatio = 0.0f;      // |residual| / local MAD, informational (not a probability)
};

// Scans the whole of `mono` and returns every residual spike, without regard to musical onsets —
// callers combine this with rejectOnsetCoincidences() before treating results as defects.
[[nodiscard]] std::vector<ClickCandidate> detectClicks(std::span<const Sample> mono, SampleRate sampleRate,
                                                          ClickDetectorConfig config = {});

// Drops candidates within `toleranceMs` of any time in `onsetTimesSeconds` (doc: "a snare hit is
// also unpredictable" — reject rather than let genuine percussion mis-flag as a click).
[[nodiscard]] std::vector<ClickCandidate> rejectOnsetCoincidences(std::vector<ClickCandidate> candidates,
                                                                    SampleRate sampleRate,
                                                                    std::span<const double> onsetTimesSeconds,
                                                                    double toleranceMs = 5.0);

}  // namespace aud::transients

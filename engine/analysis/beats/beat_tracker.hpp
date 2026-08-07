#pragma once

// M13's beat tracking ("Decision — the Ellis 2007 approach"): dynamic programming over the ODF
// given a target tempo, maximising a score that combines ODF strength at each candidate beat
// location with a penalty for deviating from the target inter-beat interval. Produces a globally
// optimal, evenly-spaced-but-onset-aligned grid — robust against a single missed or spurious onset
// in a way greedy onset-to-beat assignment isn't.

#include <cstddef>
#include <span>
#include <vector>

namespace aud::beats {

struct BeatTrackerConfig {
    // Ellis's "tightness": how strongly deviation from the target period is penalised (log-domain
    // Gaussian). Higher == a stiffer, more metronomic grid; lower == follows the ODF more loosely.
    double tightness = 100.0;
};

struct BeatTrackResult {
    std::vector<std::size_t> beatFrames;      // ODF frame index of each beat, ascending
    std::vector<float>       beatStrengths;    // ODF value (post sub-frame refinement) at each beat
    float                    phaseConfidence = 0.0f;  // 0..1, how well beats land on strong ODF peaks
};

// `odf` should be the normalised ODF (non-negative-leaning is not required — the DP only compares
// relative values). `periodSeconds` is the target inter-beat interval (60/bpm).
[[nodiscard]] BeatTrackResult trackBeats(std::span<const float> odf, double hopSeconds, double periodSeconds,
                                           BeatTrackerConfig config = {});

}  // namespace aud::beats

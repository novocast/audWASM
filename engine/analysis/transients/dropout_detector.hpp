#pragma once

// M14's dropout detector — see documentation/tasks/M14-transient-detection.md "Defect detection" /
// "Dropouts". Short runs of digital silence or dramatically reduced level in the middle of content,
// at a much shorter time scale (1-50ms) than M10's silence detector, whose minDurationMs default of
// 500ms deliberately excludes exactly this range. A run in this range is a defect, not silence.
//
// Deliberately independent of M10's SilenceDetector rather than reusing it with a smaller
// minDurationMs: M10 operates on pre-reduced 50ms RMS windows (silence_detector.hpp's header
// comment), which cannot resolve a 1-2ms dropout at all — this needs sample-accurate PCM, same as
// the rest of this module.

#include <cstddef>
#include <span>
#include <vector>

#include "../../util/audio_types.hpp"

namespace aud::transients {

struct DropoutDetectorConfig {
    double thresholdDbfs = -80.0;  // at/below this counts as "near-zero"
    double minDurationMs = 1.0;
    double maxDurationMs = 50.0;   // beyond this, M10's silence detector is the right tool
};

struct DropoutRun {
    FrameIndex begin = 0;  // inclusive
    FrameIndex end   = 0;  // exclusive
};

// `mono` is a single channel's (or the mono-mixdown's) PCM; run per channel if per-channel dropouts
// matter to the caller.
[[nodiscard]] std::vector<DropoutRun> detectDropouts(std::span<const Sample> mono, SampleRate sampleRate,
                                                        DropoutDetectorConfig config = {});

}  // namespace aud::transients

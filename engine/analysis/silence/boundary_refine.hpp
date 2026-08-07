#pragma once

// M10 "Boundary refinement": the 50ms window grid silence_detector.cpp works on is too coarse for
// a reported timestamp. Once the coarse pass has found regions, this does a sample-level scan
// within ±1 window of each boundary to find the exact first/last sample crossing the threshold —
// cheap (a few thousand samples per boundary), and only touches PCM near boundaries plus inside
// silent regions (for the level stats), never the whole file.
//
// Deliberately separate from detect(): M10's "re-running on parameter change" design keeps the
// window-grid pass instant (for live slider dragging) and defers this PCM-touching step until the
// user stops dragging (debounced by the caller).

#include <vector>

#include "../../util/audio_buffer.hpp"
#include "silence_detector.hpp"

namespace aud::silence {

// Refines every region's `range`/`startSeconds`/`endSeconds` to sample precision and recomputes
// `peakDbfsWithin`/`rmsDbfsWithin` from the actual samples in the (refined) region. `windowFrames`
// must match the grid the regions were detected on. `thresholdLinear` is the linear equivalent of
// the enter threshold used to detect them; pass 0.0 for digital silence (exact-zero test).
// No-op for SilenceKind::Perceptual regions — there is no per-sample amplitude test in the
// loudness domain, so their window-grid boundaries and LUFS-based level stand as reported.
[[nodiscard]] Result<void> refineRegionBoundaries(const AudioBuffer& buffer, std::vector<SilenceRegion>& regions,
                                                    std::size_t windowFrames, double thresholdLinear,
                                                    ChannelMode channelMode);

// Nearest zero crossing to `frame` on channel `ch`, searching outward within ±searchRadius frames.
// Returns `frame` unchanged if no crossing is found in range (e.g. sustained DC) or the arguments
// are out of bounds. Shared with future trim/export features (M10: "shared with future trim/export
// features") as well as the "suggest trim points" action.
[[nodiscard]] FrameIndex nearestZeroCrossing(const AudioBuffer& buffer, ChannelIndex ch, FrameIndex frame,
                                              FrameIndex searchRadius);

}  // namespace aud::silence

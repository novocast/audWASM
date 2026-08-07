#pragma once

// Exact cursor-readout query (M07 "Cursor readout"): a single centred STFT frame at the hovered
// time, quadratic peak interpolation (M06's peak_interp.hpp) around the bin nearest the hovered
// frequency, so hovering near a peak reports the true peak frequency rather than the bin centre.
// Deliberately bypasses the (lossy, 8-bit quantised) tile cache entirely — this is what makes the
// spectrogram a measurement instrument rather than just a picture, per the milestone.

#include "../util/audio_buffer.hpp"
#include "../util/result.hpp"
#include "tile.hpp"

namespace aud::spectrogram {

struct PointResult {
    double frequencyHz = 0.0;  // peak-interpolated
    double magnitudeDb = 0.0;
};

// `targetHz` is the frequency the cursor is nearest (e.g. from the row under the pointer via
// FreqMapping::nearestRow()/row().centerHz) — the search for the true peak happens in a small bin
// neighbourhood around it, not across the whole spectrum, since a spectrogram can have many local
// maxima and the user is pointing at a specific one.
[[nodiscard]] Result<PointResult> queryPoint(const AudioBuffer& buffer, ChannelIndex channel,
                                              double timeSeconds, double targetHz, const TileConfig& config);

}  // namespace aud::spectrogram

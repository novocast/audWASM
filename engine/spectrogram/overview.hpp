#pragma once

// The eager, always-resident, whole-track overview strip (M07 "The overview level"). Computed once
// per channel right after decode (rides along "for free" alongside it — one extra large-hop STFT
// pass, cheap relative to decode itself) so there's an instant image on load, a minimap/scrollbar
// background, and something other than empty space while the real tiles stream in.

#include <cstdint>
#include <vector>

#include "../util/audio_buffer.hpp"
#include "../util/result.hpp"
#include "tile.hpp"

namespace aud::spectrogram {

struct OverviewStrip {
    std::uint32_t         width  = 0;
    std::uint32_t         height = 0;
    float                 floorDb = -96.0f;
    float                 ceilDb  = 0.0f;
    std::vector<std::uint8_t> pixels;  // [height][width], row 0 = lowest frequency, same convention as TileData
};

// `width`/`height` default to the milestone's stated "typically 2048x256 pixels for the whole
// track". One frame is sampled per output column (a single very-large-hop STFT pass), not folded —
// per the design doc this is deliberately the cheap, coarse path; it is not meant to catch every
// transient, the real tile pyramid is.
[[nodiscard]] Result<OverviewStrip> computeOverviewStrip(const AudioBuffer& buffer, ChannelIndex channel,
                                                          const TileConfig& config, std::uint32_t width = 2048,
                                                          std::uint32_t height = kTileHeight);

}  // namespace aud::spectrogram

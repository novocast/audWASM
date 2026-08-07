#pragma once

// PCM range -> STFT -> dB -> freq-map -> quantise -> one 64KB tile (M07 "Tile pipeline",
// "Time resolution levels"). Levels 0-2 sample directly at that level's hop; levels 3+ sample at
// level 2's hop over the wider time span the coarser tile covers and MAX-fold (default; Mean is a
// user toggle) groups of frames into each output column — decimating by max, never by recomputing
// with a bigger hop, so a transient/click stays visible at every zoom level (M07 decision: taking
// the max preserves transients the same way the waveform min/max pyramid does; a bigger hop would
// just skip audio and make short events flicker in and out as you zoom).

#include <cstddef>

#include "../util/audio_buffer.hpp"
#include "../util/result.hpp"
#include "frame_computer.hpp"
#include "freq_mapping.hpp"
#include "tile.hpp"

namespace aud::spectrogram {

// hop, in samples, for `level`'s STFT columns: fftSize/4 (level 0, 75% overlap), fftSize/2 (1),
// fftSize (2), and fftSize again for every level >= 3 (those levels differ only in fold factor,
// not hop — see foldFactorForLevel()).
[[nodiscard]] inline std::size_t hopForLevel(std::size_t fftSize, std::uint32_t level) noexcept {
    if (level == 0) return fftSize / 4;
    if (level == 1) return fftSize / 2;
    return fftSize;
}

// How many level-2-hop columns get folded (max/mean) into one output column at `level`. 1 for
// levels 0-2 (no folding); 2^(level-2) for level >= 3.
[[nodiscard]] inline std::size_t foldFactorForLevel(std::uint32_t level) noexcept {
    return level <= 2 ? std::size_t{1} : (std::size_t{1} << (level - 2));
}

class TileGenerator {
public:
    static Result<TileGenerator> create(const TileConfig& config, SampleRate sampleRate);

    // `key.configHash` must match this generator's config (computeConfigHash(config, sampleRate));
    // callers (the tile cache) are responsible for recreating the generator when config changes.
    [[nodiscard]] Result<TileData> generate(const AudioBuffer& buffer, const TileKey& key);

    [[nodiscard]] const TileConfig& config() const noexcept { return m_config; }
    [[nodiscard]] SampleRate        sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] const FreqMapping& freqMapping() const noexcept { return m_mapping; }

private:
    TileGenerator(TileConfig config, SampleRate sampleRate, FrameComputer frameComputer, FreqMapping mapping)
        : m_config(config), m_sampleRate(sampleRate), m_frameComputer(std::move(frameComputer)),
          m_mapping(std::move(mapping)) {}

    TileConfig    m_config;
    SampleRate    m_sampleRate;
    FrameComputer m_frameComputer;
    FreqMapping   m_mapping;
};

}  // namespace aud::spectrogram

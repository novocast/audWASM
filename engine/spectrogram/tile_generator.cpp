#include "tile_generator.hpp"

#include <algorithm>
#include <array>

namespace aud::spectrogram {

Result<TileGenerator> TileGenerator::create(const TileConfig& config, SampleRate sampleRate) {
    AUD_TRY_ASSIGN(frameComputer,
                   FrameComputer::create(config.fftSize, static_cast<aud::fft::WindowType>(config.window),
                                          static_cast<aud::fft::SpectrumScaling>(config.scaling), sampleRate));
    AUD_TRY_ASSIGN(mapping, FreqMapping::create(static_cast<FreqAxis>(config.freqAxis), config.minHz,
                                                 static_cast<double>(sampleRate), frameComputer.binCount(),
                                                 kTileHeight));
    return TileGenerator(config, sampleRate, std::move(frameComputer), std::move(mapping));
}

Result<TileData> TileGenerator::generate(const AudioBuffer& buffer, const TileKey& key) {
    if (key.configHash != computeConfigHash(m_config, m_sampleRate)) {
        return Error{ErrorCode::CacheVersionMismatch, "spectrogram.tile_generator", "stale configHash"};
    }
    if (key.channel >= buffer.channelCount()) {
        return Error{ErrorCode::InvalidArgument, "spectrogram.tile_generator", "channel out of range"};
    }

    const std::size_t hop  = hopForLevel(m_config.fftSize, key.level);
    const std::size_t fold = foldFactorForLevel(key.level);
    const auto        decimation = static_cast<Decimation>(m_config.decimation);

    const auto baseFrame = static_cast<FrameIndex>(key.tileX) * static_cast<FrameIndex>(kTileWidth) *
                            static_cast<FrameIndex>(fold);

    TileData tile;
    tile.key        = key;
    tile.floorDb    = m_config.floorDb;
    tile.ceilDb     = m_config.ceilDb;
    tile.decimation = m_config.decimation;

    std::array<double, kTileHeight> rowScratch{};
    std::array<double, kTileHeight> foldAccum{};

    for (std::size_t outCol = 0; outCol < kTileWidth; ++outCol) {
        for (std::size_t f = 0; f < fold; ++f) {
            const auto rawCol       = static_cast<FrameIndex>(outCol * fold + f);
            const FrameIndex centerSample = (baseFrame + rawCol) * static_cast<FrameIndex>(hop);

            const auto magnitudes = m_frameComputer.computeFrame(buffer, key.channel, centerSample);
            m_mapping.mapToDb(magnitudes, rowScratch);

            if (f == 0) {
                foldAccum = rowScratch;
            } else if (decimation == Decimation::Max) {
                for (std::size_t row = 0; row < kTileHeight; ++row) {
                    foldAccum[row] = std::max(foldAccum[row], rowScratch[row]);
                }
            } else {
                for (std::size_t row = 0; row < kTileHeight; ++row) {
                    foldAccum[row] += rowScratch[row];
                }
            }
        }

        if (fold > 1 && decimation == Decimation::Mean) {
            for (std::size_t row = 0; row < kTileHeight; ++row) {
                foldAccum[row] /= static_cast<double>(fold);
            }
        }

        for (std::size_t row = 0; row < kTileHeight; ++row) {
            tile.pixels[row * kTileWidth + outCol] = quantiseDb(foldAccum[row], tile.floorDb, tile.ceilDb);
        }
    }

    return tile;
}

}  // namespace aud::spectrogram

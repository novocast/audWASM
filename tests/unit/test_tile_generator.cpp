#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "../../engine/spectrogram/tile.hpp"
#include "../../engine/spectrogram/tile_generator.hpp"
#include "../../engine/util/audio_buffer.hpp"

using aud::AudioBuffer;
using aud::Sample;
using aud::spectrogram::Decimation;
using aud::spectrogram::foldFactorForLevel;
using aud::spectrogram::hopForLevel;
using aud::spectrogram::kTileHeight;
using aud::spectrogram::kTileWidth;
using aud::spectrogram::TileConfig;
using aud::spectrogram::TileGenerator;
using aud::spectrogram::TileKey;

namespace {

constexpr aud::SampleRate kSampleRate  = 44100;
constexpr std::size_t     kFftSize     = 256;
constexpr std::size_t     kBufferFrames = 20000;
constexpr std::size_t     kBurstStart   = 5000;
constexpr std::size_t     kBurstLen     = 50;

AudioBuffer makeBufferWithBurst() {
    auto result = AudioBuffer::create(kSampleRate, 1);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();

    std::vector<Sample> samples(kBufferFrames, 0.0f);
    for (std::size_t i = kBurstStart; i < kBurstStart + kBurstLen; ++i) {
        samples[i] = 1.0f;
    }
    std::vector<std::span<const Sample>> planar{samples};
    REQUIRE(buffer.append(planar, kBufferFrames).has_value());
    return buffer;
}

// Loudest pixel value (across all rows) in a tile's column, searched in a small window around the
// column the burst is expected to land in.
std::uint8_t peakNear(const aud::spectrogram::TileData& tile, std::size_t expectedCol) {
    std::uint8_t best = 0;
    const std::size_t lo = expectedCol > 5 ? expectedCol - 5 : 0;
    const std::size_t hi = std::min(expectedCol + 5, static_cast<std::size_t>(kTileWidth - 1));
    for (std::size_t col = lo; col <= hi; ++col) {
        for (std::size_t row = 0; row < kTileHeight; ++row) {
            best = std::max(best, tile.pixels[row * kTileWidth + col]);
        }
    }
    return best;
}

// Baseline quiet-region peak, taken from columns far from the burst (columns 200-255, always well
// past sample 5000 for every hop used in this test).
std::uint8_t quietBaseline(const aud::spectrogram::TileData& tile) {
    std::uint8_t best = 0;
    for (std::size_t col = 200; col < kTileWidth; ++col) {
        for (std::size_t row = 0; row < kTileHeight; ++row) {
            best = std::max(best, tile.pixels[row * kTileWidth + col]);
        }
    }
    return best;
}

TileConfig makeConfig(Decimation decimation) {
    TileConfig config;
    config.fftSize    = kFftSize;
    config.window      = 0;  // Rectangular — a clean broadband pulse doesn't need spectral leakage control
    config.scaling     = 1;  // Amplitude
    config.freqAxis    = 0;  // Linear
    config.decimation  = static_cast<std::uint32_t>(decimation);
    config.minHz       = 20.0f;
    config.floorDb     = -96.0f;
    config.ceilDb      = 0.0f;
    return config;
}

}  // namespace

TEST_CASE("TileGenerator: an isolated click remains visible at every time-resolution level (max-decimation guard)",
          "[spectrogram][tile_generator]") {
    auto buffer = makeBufferWithBurst();
    auto configResult = TileGenerator::create(makeConfig(Decimation::Max), kSampleRate);
    REQUIRE(configResult.has_value());
    auto& generator = configResult.value();

    for (std::uint32_t level = 0; level <= 4; ++level) {
        TileKey key;
        key.level      = level;
        key.tileX      = 0;
        key.channel     = 0;
        key.configHash  = aud::spectrogram::computeConfigHash(makeConfig(Decimation::Max), kSampleRate);

        auto tileResult = generator.generate(buffer, key);
        REQUIRE(tileResult.has_value());
        const auto& tile = tileResult.value();

        const std::size_t hop         = hopForLevel(kFftSize, level);
        const std::size_t fold        = foldFactorForLevel(level);
        const std::size_t expectedCol = (kBurstStart / hop) / std::max<std::size_t>(fold, 1);

        const std::uint8_t burstPeak = peakNear(tile, expectedCol);
        const std::uint8_t baseline  = quietBaseline(tile);

        CAPTURE(level, hop, fold, expectedCol, static_cast<int>(burstPeak), static_cast<int>(baseline));
        // The click must clearly stand out from silence at every level — this is the guard against
        // "coarse levels recompute with a bigger hop and skip the click entirely".
        REQUIRE(burstPeak > baseline + 20);
    }
}

TEST_CASE("TileGenerator: Max decimation preserves the click better than Mean once folding kicks in",
          "[spectrogram][tile_generator]") {
    auto buffer = makeBufferWithBurst();

    auto maxGenResult  = TileGenerator::create(makeConfig(Decimation::Max), kSampleRate);
    auto meanGenResult = TileGenerator::create(makeConfig(Decimation::Mean), kSampleRate);
    REQUIRE(maxGenResult.has_value());
    REQUIRE(meanGenResult.has_value());

    constexpr std::uint32_t kLevel = 4;  // fold factor 4 — folding is actually happening

    TileKey maxKey;
    maxKey.level      = kLevel;
    maxKey.configHash  = aud::spectrogram::computeConfigHash(makeConfig(Decimation::Max), kSampleRate);
    TileKey meanKey       = maxKey;
    meanKey.configHash    = aud::spectrogram::computeConfigHash(makeConfig(Decimation::Mean), kSampleRate);

    auto maxTile  = maxGenResult.value().generate(buffer, maxKey);
    auto meanTile = meanGenResult.value().generate(buffer, meanKey);
    REQUIRE(maxTile.has_value());
    REQUIRE(meanTile.has_value());

    const std::size_t hop         = hopForLevel(kFftSize, kLevel);
    const std::size_t fold        = foldFactorForLevel(kLevel);
    const std::size_t expectedCol = (kBurstStart / hop) / fold;

    const std::uint8_t maxPeak  = peakNear(maxTile.value(), expectedCol);
    const std::uint8_t meanPeak = peakNear(meanTile.value(), expectedCol);

    CAPTURE(fold, expectedCol, static_cast<int>(maxPeak), static_cast<int>(meanPeak));
    REQUIRE(static_cast<int>(maxPeak) >= static_cast<int>(meanPeak));
}

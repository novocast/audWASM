#pragma once

// Tile identity, configuration hashing, and 8-bit dB quantisation (M07 "Tiles"). A tile is a fixed
// 256x256 grid of quantised dB bytes covering `kTileWidth` STFT columns by `kTileHeight` display
// frequency rows (see freq_mapping.hpp for the bin->row mapping) — never floats (M07 decision: a
// 60 dB display range quantised to 256 levels is 0.24 dB/step, below what a colour map or a human
// eye resolves, and quantising once here is what lets colour map/gain/gamma/floor change be a
// shader-only, no-regeneration operation).
//
// Row convention: row 0 is the lowest displayed frequency, row (kTileHeight-1) the highest —
// callers that want a conventional top-down image (high frequency at the top) flip when uploading
// to a texture/atlas, not here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../util/audio_types.hpp"

namespace aud::spectrogram {

inline constexpr std::uint32_t kTileWidth  = 256;  // STFT frame columns
inline constexpr std::uint32_t kTileHeight = 256;  // output frequency rows (post freq-mapping)

enum class FreqAxis : std::uint32_t {
    Linear,
    Log,
    Mel,
    Bark,
};

// How coarse time-resolution levels (M07 "Time resolution levels", level >= 3) fold multiple finer
// frames into one output column. Max preserves transients at every zoom (never missing a click);
// Mean is offered because Max makes a very coarse overview look uniformly loud.
enum class Decimation : std::uint32_t {
    Max,
    Mean,
};

// Every parameter that changes what a tile's pixels mean must live here — `computeConfigHash()`
// hashes exactly these bytes, and the static_assert below is the M07 "configHash misses a
// parameter" mitigation: widening this struct without updating the hash call site fails to compile
// only if you forget to bump the assert, which is a single, obvious, one-line diff to review.
struct TileConfig {
    std::uint32_t fftSize    = 4096;
    std::uint32_t window     = 1;  // aud::fft::WindowType, default Hann
    std::uint32_t scaling    = 1;  // aud::fft::SpectrumScaling, default Amplitude
    std::uint32_t freqAxis   = static_cast<std::uint32_t>(FreqAxis::Log);
    std::uint32_t decimation = static_cast<std::uint32_t>(Decimation::Max);
    float         minHz      = 20.0f;   // log/mel/bark axis floor
    float         floorDb    = -96.0f;  // quantisation range floor
    float         ceilDb     = 0.0f;    // quantisation range ceiling

    friend bool operator==(const TileConfig&, const TileConfig&) = default;
};
static_assert(sizeof(TileConfig) == 8 * 4,
              "TileConfig grew/shrank — computeConfigHash() hashes sizeof(TileConfig) raw bytes; "
              "adding a field is safe (it's included automatically), but double check padding "
              "didn't change and bump this assert.");

// FNV-1a over TileConfig's raw bytes plus sampleRate (a tile computed against one file's sample
// rate is meaningless for another). Two different in-memory TileConfig values with the same bytes
// always hash identically — that's the whole point of storing it as one packed struct rather than
// hashing fields ad hoc.
[[nodiscard]] inline std::uint32_t computeConfigHash(const TileConfig& config, SampleRate sampleRate) noexcept {
    std::uint32_t hash = 2166136261u;
    auto          mix  = [&hash](const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 16777619u;
        }
    };
    mix(&config, sizeof(config));
    mix(&sampleRate, sizeof(sampleRate));
    return hash;
}

struct TileKey {
    std::uint32_t level      = 0;  // time resolution level, 0 = finest
    std::uint32_t tileX      = 0;  // time index at this level
    ChannelIndex  channel    = 0;
    std::uint32_t configHash = 0;

    friend bool operator==(const TileKey&, const TileKey&) = default;
};

// [floorDb, ceilDb] -> [0, 255]. `db` outside the range clamps; this is the only place a tile's
// bytes and real dB values are converted between each other (round-trip error < 0.25 dB per the
// M07 unit test, i.e. well under a colour map's resolution).
[[nodiscard]] inline std::uint8_t quantiseDb(double db, double floorDb, double ceilDb) noexcept {
    const double range = ceilDb - floorDb;
    if (range <= 0.0) {
        return 0;
    }
    double t = (db - floorDb) / range;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return static_cast<std::uint8_t>(t * 255.0 + 0.5);
}

[[nodiscard]] inline double dequantiseDb(std::uint8_t value, double floorDb, double ceilDb) noexcept {
    return floorDb + (static_cast<double>(value) / 255.0) * (ceilDb - floorDb);
}

// A generated tile: kTileWidth*kTileHeight quantised bytes plus the header needed to dequantise
// them (floorDb/ceilDb travel with the tile rather than being looked up elsewhere, since a cache
// holding tiles from a config that has since changed must still be able to render itself correctly
// until it's evicted/invalidated).
struct TileData {
    TileKey                    key;
    float                      floorDb = -96.0f;
    float                      ceilDb  = 0.0f;
    std::uint32_t              decimation = static_cast<std::uint32_t>(Decimation::Max);
    std::array<std::uint8_t, static_cast<std::size_t>(kTileWidth) * kTileHeight> pixels{};

    [[nodiscard]] static constexpr std::size_t byteSize() noexcept {
        return sizeof(TileData);
    }
};

}  // namespace aud::spectrogram

namespace std {
template <>
struct hash<aud::spectrogram::TileKey> {
    std::size_t operator()(const aud::spectrogram::TileKey& key) const noexcept {
        // FNV-1a in 64-bit arithmetic regardless of std::size_t's actual width (32-bit on WASM) —
        // only the final truncation to size_t at the return is platform-width-dependent, which is
        // fine for a hash (it just narrows, same as std::hash<T> generally does on 32-bit targets).
        std::uint64_t h = 1469598103934665603ull;
        auto          mix = [&h](std::uint64_t v) noexcept {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(key.level);
        mix(key.tileX);
        mix(key.channel);
        mix(key.configHash);
        return static_cast<std::size_t>(h);
    }
};
}  // namespace std

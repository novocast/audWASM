#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "../../engine/spectrogram/tile.hpp"
#include "../../engine/spectrogram/tile_cache.hpp"
#include "../../engine/util/audio_buffer.hpp"

using aud::AudioBuffer;
using aud::Sample;
using aud::spectrogram::dequantiseDb;
using aud::spectrogram::quantiseDb;
using aud::spectrogram::TileCache;
using aud::spectrogram::TileConfig;
using aud::spectrogram::TileData;
using aud::spectrogram::TileKey;
using Catch::Approx;

namespace {

constexpr aud::SampleRate kSampleRate = 44100;

AudioBuffer makeSilentBuffer(std::size_t frames) {
    auto result = AudioBuffer::create(kSampleRate, 1);
    REQUIRE(result.has_value());
    auto                 buffer = std::move(result).value();
    std::vector<Sample>  samples(frames, 0.0f);
    std::vector<std::span<const Sample>> planar{samples};
    REQUIRE(buffer.append(planar, frames).has_value());
    return buffer;
}

TileConfig makeConfig() {
    TileConfig config;
    config.fftSize = 256;
    return config;
}

}  // namespace

TEST_CASE("quantiseDb/dequantiseDb round-trip error is under 0.25 dB across the range", "[spectrogram][tile_cache]") {
    constexpr double floorDb = -96.0;
    constexpr double ceilDb  = 0.0;

    double worst = 0.0;
    for (int i = 0; i <= 2000; ++i) {
        const double db     = floorDb + (ceilDb - floorDb) * (static_cast<double>(i) / 2000.0);
        const auto   byte   = quantiseDb(db, floorDb, ceilDb);
        const double back   = dequantiseDb(byte, floorDb, ceilDb);
        worst = std::max(worst, std::abs(back - db));
    }
    REQUIRE(worst < 0.25);
}

TEST_CASE("TileCache: byte budget is respected under adversarial insertion, no unbounded growth",
          "[spectrogram][tile_cache]") {
    auto buffer = makeSilentBuffer(200000);

    // Budget for ~10 tiles; TileData::byteSize() is fixed (256*256 + header), so this is exact.
    const std::size_t budget = TileData::byteSize() * 10;
    TileCache          cache(budget);
    REQUIRE(cache.setConfig(makeConfig(), kSampleRate).has_value());

    // Request 50 distinct tiles (different tileX so each is a genuine cache miss) — an adversarial
    // fast-scroll pattern that never revisits a tile.
    for (std::uint32_t tileX = 0; tileX < 50; ++tileX) {
        TileKey key;
        key.level      = 0;
        key.tileX      = tileX;
        key.channel     = 0;
        key.configHash  = cache.currentConfigHash();

        auto result = cache.request(buffer, key);
        REQUIRE(result.has_value());
        REQUIRE(cache.currentBytes() <= cache.byteBudget());
    }

    REQUIRE(cache.currentBytes() <= budget);
    REQUIRE(cache.tileCount() <= 10);
}

TEST_CASE("TileCache: re-requesting the same key is a cache hit (no regeneration, LRU touch)",
          "[spectrogram][tile_cache]") {
    auto buffer = makeSilentBuffer(200000);
    TileCache cache(TileData::byteSize() * 4);
    REQUIRE(cache.setConfig(makeConfig(), kSampleRate).has_value());

    TileKey key;
    key.level      = 0;
    key.tileX      = 0;
    key.channel     = 0;
    key.configHash  = cache.currentConfigHash();

    auto first = cache.request(buffer, key);
    REQUIRE(first.has_value());
    const TileData* firstPtr = first.value();

    auto second = cache.request(buffer, key);
    REQUIRE(second.has_value());
    REQUIRE(second.value() == firstPtr);  // same resident entry, not regenerated
    REQUIRE(cache.tileCount() == 1);
}

TEST_CASE("TileCache: requesting a key with a stale configHash fails; invalidateConfig() frees its tiles",
          "[spectrogram][tile_cache]") {
    auto buffer = makeSilentBuffer(200000);
    TileCache cache(TileData::byteSize() * 20);

    TileConfig configA = makeConfig();
    REQUIRE(cache.setConfig(configA, kSampleRate).has_value());
    const std::uint32_t hashA = cache.currentConfigHash();

    TileKey key;
    key.level      = 0;
    key.tileX      = 0;
    key.channel     = 0;
    key.configHash  = hashA;
    REQUIRE(cache.request(buffer, key).has_value());
    REQUIRE(cache.tileCount() == 1);

    TileConfig configB = makeConfig();
    configB.fftSize    = 512;  // different config -> different hash
    REQUIRE(cache.setConfig(configB, kSampleRate).has_value());
    const std::uint32_t hashB = cache.currentConfigHash();
    REQUIRE(hashB != hashA);

    // The old key (still carrying hashA) is now stale relative to the cache's live config.
    REQUIRE_FALSE(cache.request(buffer, key).has_value());

    // But a key built with the new hash works, and the old-hash tile is still resident until
    // explicitly invalidated (M07: "old tiles shown until new ones arrive").
    TileKey keyB      = key;
    keyB.configHash    = hashB;
    REQUIRE(cache.request(buffer, keyB).has_value());
    REQUIRE(cache.tileCount() == 2);

    cache.invalidateConfig(hashA);
    REQUIRE(cache.tileCount() == 1);
}

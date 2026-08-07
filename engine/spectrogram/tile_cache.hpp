#pragma once

// LRU tile cache with a byte budget (M07 "Tiles" / acceptance criteria: "Tile cache stays within
// its configured byte budget under adversarial fast scrolling; no unbounded growth"). No generic
// LRU exists elsewhere in the engine to reuse (engine/cache is reserved for M16's on-disk .awc
// format) — this is genuinely new: an intrusive std::list for recency order (splice() doesn't
// invalidate iterators, which is what makes touch-on-hit O(1) without a second data structure) plus
// an unordered_map for O(1) key lookup.
//
// Decision — changing fftSize/window/axis/range does NOT eagerly purge old-config tiles. A new
// TileConfig produces a new configHash, so request()s for the new config simply miss and generate
// fresh entries; stale-config entries age out through ordinary LRU pressure. This is what gives the
// milestone's "old tiles shown until new ones arrive, no flash of empty" for free — the caller
// (the client-side tile manager) keeps rendering whatever old-hash tiles are already in its atlas
// while new ones stream in. invalidateConfig() exists only as an explicit "free this memory now"
// escape hatch (and for the unit test), not because correctness requires it.

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>

#include "../util/audio_buffer.hpp"
#include "../util/result.hpp"
#include "tile.hpp"
#include "tile_generator.hpp"

namespace aud::spectrogram {

inline constexpr std::size_t kDefaultTileCacheByteBudget = 128u * 1024 * 1024;  // M07 default, tunable

class TileCache {
public:
    explicit TileCache(std::size_t byteBudget = kDefaultTileCacheByteBudget) : m_byteBudget(byteBudget) {}

    // Swaps in a new generation config; subsequent request()s use it. Does not touch already-cached
    // tiles from a previous config (see class comment).
    Result<void> setConfig(const TileConfig& config, SampleRate sampleRate);

    [[nodiscard]] bool          hasConfig() const noexcept { return m_generator.has_value(); }
    [[nodiscard]] std::uint32_t currentConfigHash() const noexcept { return m_configHash; }

    // Returns the tile for `key`, generating and inserting it on a miss (evicting LRU entries until
    // it fits the byte budget first). `key.configHash` must equal currentConfigHash() — a mismatch
    // is a caller bug (requesting a stale-config key) and returns CacheVersionMismatch rather than
    // silently generating with the wrong generator. The returned pointer is valid until the entry
    // is evicted; callers needing it longer must copy.
    [[nodiscard]] Result<const TileData*> request(const AudioBuffer& buffer, const TileKey& key);

    // Explicitly drops every resident tile whose key.configHash == staleConfigHash.
    void invalidateConfig(std::uint32_t staleConfigHash);

    [[nodiscard]] std::size_t currentBytes() const noexcept { return m_currentBytes; }
    [[nodiscard]] std::size_t byteBudget() const noexcept { return m_byteBudget; }
    [[nodiscard]] std::size_t tileCount() const noexcept { return m_index.size(); }

private:
    struct Node {
        TileKey  key;
        TileData data;
    };

    void evictUntilFits(std::size_t incomingBytes);

    std::list<Node>                                   m_lru;  // front = most recently used
    std::unordered_map<TileKey, std::list<Node>::iterator> m_index;

    std::size_t                   m_byteBudget;
    std::size_t                   m_currentBytes = 0;
    std::optional<TileGenerator>  m_generator;
    std::uint32_t                 m_configHash = 0;
};

}  // namespace aud::spectrogram

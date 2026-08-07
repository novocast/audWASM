#include "tile_cache.hpp"

namespace aud::spectrogram {

Result<void> TileCache::setConfig(const TileConfig& config, SampleRate sampleRate) {
    AUD_TRY_ASSIGN(generator, TileGenerator::create(config, sampleRate));
    m_generator  = std::move(generator);
    m_configHash = computeConfigHash(config, sampleRate);
    return {};
}

Result<const TileData*> TileCache::request(const AudioBuffer& buffer, const TileKey& key) {
    if (!m_generator.has_value() || key.configHash != m_configHash) {
        return Error{ErrorCode::CacheVersionMismatch, "spectrogram.tile_cache",
                      "request() key.configHash does not match the cache's current config"};
    }

    auto found = m_index.find(key);
    if (found != m_index.end()) {
        auto it = found->second;
        if (it != m_lru.begin()) {
            m_lru.splice(m_lru.begin(), m_lru, it);  // touch: move to front, O(1), iterators stable
        }
        return &m_lru.begin()->data;
    }

    AUD_TRY_ASSIGN(tile, m_generator->generate(buffer, key));

    const std::size_t incoming = TileData::byteSize();
    evictUntilFits(incoming);

    m_lru.push_front(Node{key, std::move(tile)});
    m_index[key] = m_lru.begin();
    m_currentBytes += incoming;

    return &m_lru.begin()->data;
}

void TileCache::invalidateConfig(std::uint32_t staleConfigHash) {
    for (auto it = m_lru.begin(); it != m_lru.end();) {
        if (it->key.configHash == staleConfigHash) {
            m_index.erase(it->key);
            m_currentBytes -= TileData::byteSize();
            it = m_lru.erase(it);
        } else {
            ++it;
        }
    }
}

void TileCache::evictUntilFits(std::size_t incomingBytes) {
    while (!m_lru.empty() && m_currentBytes + incomingBytes > m_byteBudget) {
        const auto& victim = m_lru.back();
        m_index.erase(victim.key);
        m_currentBytes -= TileData::byteSize();
        m_lru.pop_back();
    }
}

}  // namespace aud::spectrogram

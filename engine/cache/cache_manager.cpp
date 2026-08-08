// Cache manager implementation.

#include "cache_manager.hpp"

#include <cstring>
#include <fstream>

namespace aud::cache {

Result<Blake3::Digest> CacheManager::hashSourceFile(const std::filesystem::path& sourcePath) {
    // TODO: Implement using vendored BLAKE3
    // For now, return a dummy hash
    Blake3 hasher;

    std::ifstream file(sourcePath, std::ios::binary);
    if (!file) {
        return Err("Failed to open source file for hashing: " + sourcePath.string());
    }

    const size_t CHUNK_SIZE = 1024 * 1024;  // 1 MB chunks
    std::vector<uint8_t> buffer(CHUNK_SIZE);

    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), CHUNK_SIZE);
        size_t bytesRead = file.gcount();
        if (bytesRead > 0) {
            hasher.update(std::span<const uint8_t>(buffer.data(), bytesRead));
        }
    }

    return hasher.finish();
}

CacheValidationResult CacheManager::validateCache(
    const std::filesystem::path& cachePath,
    const Blake3::Digest&        expectedSourceHash,
    uint64_t                     expectedSourceSize
) {
    CacheValidationResult result{};

    // Check if file exists
    if (!std::filesystem::exists(cachePath)) {
        return result;  // isValid = false, no handle
    }

    // Try to open and read header
    auto reader = AwcReader::open(cachePath);
    if (!reader.has_value()) {
        return result;  // isValid = false
    }

    // Validate source hash and size
    if (reader.value().header().sourceHash != expectedSourceHash ||
        reader.value().header().sourceSize != expectedSourceSize) {
        return result;  // Cache mismatch
    }

    // Valid cache
    result.isValid = true;
    result.sourceHash = expectedSourceHash;
    result.handle = CacheHandle{};
    result.handle->m_reader = std::make_shared<AwcReader>(std::move(reader.value()));

    return result;
}

Result<std::vector<uint8_t>> CacheManager::loadChunk(
    const CacheHandle&  cache,
    std::string_view    chunkType,
    uint32_t            expectedAnalyzerVersion,
    uint64_t            expectedParamsHash
) {
    if (!cache.isValid()) {
        return Ok(std::vector<uint8_t>{});  // Not found is not an error
    }

    const auto& reader = *cache.m_reader;
    const ChunkInfo* info = reader.findChunk(chunkType);
    if (!info) {
        return Ok(std::vector<uint8_t>{});  // Chunk not found
    }

    // Check analyzer version and params hash
    if (info->analyzerVersion != expectedAnalyzerVersion ||
        info->paramsHash != expectedParamsHash) {
        return Ok(std::vector<uint8_t>{});  // Chunk invalid due to version/params mismatch
    }

    // Load the chunk
    return const_cast<AwcReader*>(&reader)->loadChunk(chunkType);
}

uint64_t CacheManager::hashParameters(std::span<const uint64_t> parameterHashes) {
    uint64_t result = 0;
    for (uint64_t hash : parameterHashes) {
        result ^= hash;  // XOR to combine hashes
    }
    return result;
}

uint64_t CacheManager::hashParameter(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return xxHash3U64(bits);
}

uint64_t CacheManager::hashParameter(uint32_t value) {
    return xxHash3U64(static_cast<uint64_t>(value));
}

uint64_t CacheManager::hashParameter(std::string_view value) {
    return xxHash3(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(value.data()),
        value.size()
    ));
}

}  // namespace aud::cache

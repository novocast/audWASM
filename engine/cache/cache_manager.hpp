#pragma once

// Cache manager: coordinates cache validation, lookup, and storage.
// Responsible for:
//   - Computing source file hash (cache key)
//   - Checking cache validity (file exists, header valid, chunks match current analyzer versions)
//   - Per-chunk invalidation based on analyzer version and parameter changes
//   - Loading cached chunks into analysis results
//   - Populating a cache writer with results for storage
//
// See M16 design.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

#include "../util/audio_types.hpp"
#include "../util/result.hpp"
#include "awc_reader.hpp"
#include "awc_writer.hpp"
#include "hash.hpp"

namespace aud::cache {

// Opaque handle to a loaded cache for a file. Callers don't interact with this directly;
// it's passed to getCachedChunk() to retrieve specific payloads.
class CacheHandle {
public:
    // Check if this handle is valid (the cache file exists and is readable).
    [[nodiscard]] bool isValid() const { return m_reader != nullptr; }

    // Internal: for use by cache manager only.
    friend class CacheManager;

private:
    std::shared_ptr<AwcReader> m_reader;
};

struct CacheValidationResult {
    bool                isValid;  // true if the cache file exists, header is valid, and we can read it
    Blake3::Digest      sourceHash;
    std::optional<CacheHandle> handle;  // non-empty if valid
};

class CacheManager {
public:
    // Compute the BLAKE3 hash of a file on disk. Called once per file load; can be done
    // concurrently with the first audio decode chunks (M16 strategy).
    [[nodiscard]] static Result<Blake3::Digest> hashSourceFile(const std::filesystem::path& sourcePath);

    // Check if a cache file exists and is valid for the given source file.
    // The returned handle can be used to load individual chunks.
    [[nodiscard]] static CacheValidationResult validateCache(
        const std::filesystem::path& cachePath,
        const Blake3::Digest&        expectedSourceHash,
        uint64_t                     expectedSourceSize
    );

    // Load a chunk from a cache, if valid. The chunk's analyzerVersion and paramsHash are checked.
    // Returns the raw payload if valid, or an empty vector if the chunk is not found / invalid.
    [[nodiscard]] static Result<std::vector<uint8_t>> loadChunk(
        const CacheHandle&  cache,
        std::string_view    chunkType,
        uint32_t            expectedAnalyzerVersion,
        uint64_t            expectedParamsHash
    );

    // Compute parameter hash for an analyser. This is XORed from the individual parameter hashes.
    // For analysers with no parameters, return 0.
    [[nodiscard]] static uint64_t hashParameters(std::span<const uint64_t> parameterHashes);

    // Helper: hash a single parameter value.
    [[nodiscard]] static uint64_t hashParameter(double value);
    [[nodiscard]] static uint64_t hashParameter(uint32_t value);
    [[nodiscard]] static uint64_t hashParameter(std::string_view value);
};

}  // namespace aud::cache

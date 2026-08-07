#pragma once

// .awc file reader: validates and extracts cached analysis results.
// See M16 design and docs/awc-format.md for the format specification.
//
// Usage:
//   auto reader = AwcReader::open(filePath);
//   if (reader.ok) {
//       auto header = reader.value.header();
//       auto wvpyChunk = reader.value.loadChunk("WVPY");
//       auto lufsChunk = reader.value.loadChunk("LUFS");
//   }

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../util/audio_types.hpp"
#include "../util/result.hpp"
#include "hash.hpp"

namespace aud::cache {

struct AwcHeader {
    std::array<uint8_t, 32> sourceHash;
    uint64_t                sourceSize;
    SampleRate              sampleRate;
    ChannelIndex            channels;
    FrameIndex              frameCount;
    uint32_t                chunkCount;
    uint32_t                engineVersion;
    uint16_t                formatVersion;
};

struct ChunkInfo {
    std::array<char, 4> type;           // FourCC
    uint32_t            analyzerVersion;
    uint64_t            paramsHash;
    uint64_t            offset;
    uint64_t            storedSize;
    uint64_t            rawSize;
    uint64_t            checksum;
};

class AwcReader {
public:
    // Open and parse the header and chunk directory.
    // Returns a reader if the header is valid and parseable; returns Err otherwise.
    [[nodiscard]] static Result<AwcReader> open(const std::filesystem::path& path);

    // Get the header information.
    [[nodiscard]] const AwcHeader& header() const { return m_header; }

    // Get the list of available chunks.
    [[nodiscard]] const std::vector<ChunkInfo>& chunks() const { return m_chunks; }

    // Find a chunk by type. Returns nullptr if not found.
    [[nodiscard]] const ChunkInfo* findChunk(std::string_view type) const;

    // Load a chunk's raw payload. Returns empty vector if not found or on read error.
    [[nodiscard]] Result<std::vector<uint8_t>> loadChunk(std::string_view type);

    // Load all chunks of a given type (there may be multiple).
    [[nodiscard]] Result<std::vector<std::vector<uint8_t>>> loadChunksOfType(std::string_view type);

private:
    explicit AwcReader(const std::filesystem::path& path)
        : m_path(path) {}

    // Internal: read and validate the header and directory.
    Result<void> loadHeaderAndDirectory();

    std::filesystem::path       m_path;
    AwcHeader                   m_header{};
    std::vector<ChunkInfo>      m_chunks;
};

}  // namespace aud::cache

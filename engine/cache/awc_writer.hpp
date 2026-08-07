#pragma once

// .awc file writer: serializes analysis results to disk in the cache format.
// See M16 design and docs/awc-format.md for the format specification.
//
// Usage:
//   AwcWriter writer;
//   writer.setSourceInfo(sourceHash, sourceSize, sampleRate, channels, frameCount);
//   writer.addChunk("WVPY", analyzerVersion, paramsHash, waveformPayload);
//   writer.addChunk("LUFS", analyzerVersion, paramsHash, loudnessPayload);
//   auto result = writer.writeTo(outputPath);

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../util/audio_types.hpp"
#include "../util/result.hpp"
#include "hash.hpp"

namespace aud::cache {

struct ChunkDescriptor {
    std::array<char, 4>     type;       // FourCC, e.g., "WVPY"
    uint32_t                analyzerVersion;
    uint64_t                paramsHash;
    std::vector<uint8_t>    payload;
};

class AwcWriter {
public:
    AwcWriter() = default;

    // Set the source file metadata. Must be called before adding chunks.
    void setSourceInfo(
        const Blake3::Digest& sourceHash,
        uint64_t              sourceSize,
        SampleRate            sampleRate,
        ChannelIndex          channels,
        FrameIndex            frameCount
    );

    // Add a chunk. `payload` is moved into the writer.
    void addChunk(
        const char*                    type,      // FourCC, e.g., "WVPY"
        uint32_t                       analyzerVersion,
        uint64_t                       paramsHash,
        std::vector<uint8_t>           payload
    );

    // Write the complete .awc file to disk. Writes to a temp file and renames atomically.
    [[nodiscard]] Result<void> writeTo(const std::filesystem::path& path);

private:
    Blake3::Digest           m_sourceHash;
    uint64_t                 m_sourceSize     = 0;
    SampleRate               m_sampleRate     = 0;
    ChannelIndex             m_channels       = 0;
    FrameIndex               m_frameCount     = 0;
    std::vector<ChunkDescriptor> m_chunks;
};

}  // namespace aud::cache

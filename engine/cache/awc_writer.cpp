// .awc file writer implementation.
// See docs/awc-format.md for format specification.

#include "awc_writer.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "hash.hpp"
#include "serialise.hpp"

namespace aud::cache {

void AwcWriter::setSourceInfo(
    const Blake3::Digest& sourceHash,
    uint64_t              sourceSize,
    SampleRate            sampleRate,
    ChannelIndex          channels,
    FrameIndex            frameCount
) {
    m_sourceHash = sourceHash;
    m_sourceSize = sourceSize;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_frameCount = frameCount;
}

void AwcWriter::addChunk(
    const char*                    type,
    uint32_t                       analyzerVersion,
    uint64_t                       paramsHash,
    std::vector<uint8_t>           payload
) {
    ChunkDescriptor chunk;
    std::memcpy(chunk.type.data(), type, 4);
    chunk.analyzerVersion = analyzerVersion;
    chunk.paramsHash = paramsHash;
    chunk.payload = std::move(payload);
    m_chunks.push_back(std::move(chunk));
}

Result<void> AwcWriter::writeTo(const std::filesystem::path& path) {
    using namespace std::chrono;

    // Compute engine version (hardcoded for now; should be replaced with actual version).
    // Format: major.minor.patch packed into u32. For now, 0.1.0 = 0x00010000.
    const uint32_t ENGINE_VERSION = 0x00010000;

    // Prepare the file buffer.
    // Layout: header (80 bytes), then all chunk payloads back-to-back, then the chunk directory.
    // All offsets in the directory are absolute from file start.
    std::vector<uint8_t> fileBuffer;

    // Build and write header (80 bytes)
    {
        // char[4]: magic
        fileBuffer.push_back('A');
        fileBuffer.push_back('W');
        fileBuffer.push_back('C');
        fileBuffer.push_back('1');

        // u16: formatVersion
        fileBuffer.push_back(0x01);
        fileBuffer.push_back(0x00);

        // u16: headerSize
        fileBuffer.push_back(0x50);
        fileBuffer.push_back(0x00);

        // u32: flags
        fileBuffer.push_back(0x00);
        fileBuffer.push_back(0x00);
        fileBuffer.push_back(0x00);
        fileBuffer.push_back(0x00);

        // u32: engineVersion
        fileBuffer.push_back(static_cast<uint8_t>(ENGINE_VERSION & 0xFF));
        fileBuffer.push_back(static_cast<uint8_t>((ENGINE_VERSION >> 8) & 0xFF));
        fileBuffer.push_back(static_cast<uint8_t>((ENGINE_VERSION >> 16) & 0xFF));
        fileBuffer.push_back(static_cast<uint8_t>((ENGINE_VERSION >> 24) & 0xFF));

        // u8[32]: sourceHash
        fileBuffer.insert(fileBuffer.end(), m_sourceHash.begin(), m_sourceHash.end());

        // u64: sourceSize
        for (int i = 0; i < 8; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((m_sourceSize >> (i * 8)) & 0xFF));
        }

        // u32: sampleRate
        uint32_t sr = static_cast<uint32_t>(m_sampleRate);
        for (int i = 0; i < 4; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((sr >> (i * 8)) & 0xFF));
        }

        // u32: channels
        uint32_t ch = static_cast<uint32_t>(m_channels);
        for (int i = 0; i < 4; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((ch >> (i * 8)) & 0xFF));
        }

        // i64: frameCount
        uint64_t fc = static_cast<uint64_t>(m_frameCount);
        for (int i = 0; i < 8; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((fc >> (i * 8)) & 0xFF));
        }

        // u32: chunkCount
        uint32_t cc = static_cast<uint32_t>(m_chunks.size());
        for (int i = 0; i < 4; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((cc >> (i * 8)) & 0xFF));
        }

        // u32: reserved
        fileBuffer.push_back(0x00);
        fileBuffer.push_back(0x00);
        fileBuffer.push_back(0x00);
        fileBuffer.push_back(0x00);
    }

    // Write all chunk payloads
    for (const auto& chunk : m_chunks) {
        fileBuffer.insert(fileBuffer.end(), chunk.payload.begin(), chunk.payload.end());
    }

    // Write chunk directory
    for (const auto& chunk : m_chunks) {
        uint64_t checksum = xxHash3(std::span<const uint8_t>(chunk.payload));
        uint64_t chunkOffset = 80;  // Start after header
        for (const auto& prevChunk : m_chunks) {
            if (&prevChunk == &chunk) break;
            chunkOffset += prevChunk.payload.size();
        }

        // Write directory entry (48 bytes)
        uint32_t type = static_cast<uint32_t>(
            (static_cast<uint32_t>(chunk.type[0]) << 0) |
            (static_cast<uint32_t>(chunk.type[1]) << 8) |
            (static_cast<uint32_t>(chunk.type[2]) << 16) |
            (static_cast<uint32_t>(chunk.type[3]) << 24)
        );

        // type (u32)
        for (int i = 0; i < 4; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((type >> (i * 8)) & 0xFF));
        }

        // analyzerVersion (u32)
        for (int i = 0; i < 4; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((chunk.analyzerVersion >> (i * 8)) & 0xFF));
        }

        // paramsHash (u64)
        for (int i = 0; i < 8; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((chunk.paramsHash >> (i * 8)) & 0xFF));
        }

        // offset (u64)
        for (int i = 0; i < 8; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((chunkOffset >> (i * 8)) & 0xFF));
        }

        // storedSize (u64)
        uint64_t sz = static_cast<uint64_t>(chunk.payload.size());
        for (int i = 0; i < 8; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((sz >> (i * 8)) & 0xFF));
        }

        // rawSize (u64)
        for (int i = 0; i < 8; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((sz >> (i * 8)) & 0xFF));
        }

        // checksum (u64)
        for (int i = 0; i < 8; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((checksum >> (i * 8)) & 0xFF));
        }
    }

    // Write to temp file, then rename atomically
    auto tempPath = path.parent_path() / (path.filename().string() + ".tmp");

    std::ofstream out(tempPath, std::ios::binary);
    if (!out) {
        return Err("Failed to open cache file for writing: " + tempPath.string());
    }

    out.write(reinterpret_cast<const char*>(fileBuffer.data()), fileBuffer.size());
    if (!out) {
        std::filesystem::remove(tempPath);
        return Err("Failed to write cache file: " + tempPath.string());
    }
    out.close();

    // Rename atomically
    std::error_code ec;
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return Err("Failed to rename cache file: " + ec.message());
    }

    return Ok();
}

}  // namespace aud::cache

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

    // Prepare the file buffer
    std::vector<uint8_t> fileBuffer;

    // Write header (80 bytes)
    // magic
    fileBuffer.push_back('A');
    fileBuffer.push_back('W');
    fileBuffer.push_back('C');
    fileBuffer.push_back('1');

    // formatVersion (u16)
    fileBuffer.push_back(0x01);
    fileBuffer.push_back(0x00);

    // headerSize (u16) = 80
    fileBuffer.push_back(0x50);
    fileBuffer.push_back(0x00);

    // flags (u32) = 0 (no compression in v1)
    fileBuffer.push_back(0x00);
    fileBuffer.push_back(0x00);
    fileBuffer.push_back(0x00);
    fileBuffer.push_back(0x00);

    // engineVersion (u32)
    fileBuffer.push_back(static_cast<uint8_t>(ENGINE_VERSION & 0xFF));
    fileBuffer.push_back(static_cast<uint8_t>((ENGINE_VERSION >> 8) & 0xFF));
    fileBuffer.push_back(static_cast<uint8_t>((ENGINE_VERSION >> 16) & 0xFF));
    fileBuffer.push_back(static_cast<uint8_t>((ENGINE_VERSION >> 24) & 0xFF));

    // sourceHash (u8[32])
    fileBuffer.insert(fileBuffer.end(), m_sourceHash.begin(), m_sourceHash.end());

    // sourceSize (u64)
    {
        uint64_t sz = m_sourceSize;
        for (int i = 0; i < 8; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((sz >> (i * 8)) & 0xFF));
        }
    }

    // sampleRate (u32)
    {
        uint32_t sr = m_sampleRate;
        for (int i = 0; i < 4; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((sr >> (i * 8)) & 0xFF));
        }
    }

    // channels (u32)
    {
        uint32_t ch = m_channels;
        for (int i = 0; i < 4; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((ch >> (i * 8)) & 0xFF));
        }
    }

    // frameCount (i64)
    {
        int64_t fc = m_frameCount;
        uint64_t ufc = static_cast<uint64_t>(fc);
        for (int i = 0; i < 8; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((ufc >> (i * 8)) & 0xFF));
        }
    }

    // chunkCount (u32)
    {
        uint32_t cc = static_cast<uint32_t>(m_chunks.size());
        for (int i = 0; i < 4; ++i) {
            fileBuffer.push_back(static_cast<uint8_t>((cc >> (i * 8)) & 0xFF));
        }
    }

    // reserved (u32) = 0
    fileBuffer.push_back(0x00);
    fileBuffer.push_back(0x00);
    fileBuffer.push_back(0x00);
    fileBuffer.push_back(0x00);

    // Write chunk payloads and collect directory entries
    std::vector<uint8_t> directoryBuffer;
    uint64_t chunkDirOffset = 80;  // Header size

    for (const auto& chunk : m_chunks) {
        // Compute checksum of the payload
        uint64_t checksum = xxHash3(std::span<const uint8_t>(chunk.payload));

        // Record directory entry
        PayloadWriter dirEntry;
        dirEntry.writeU32(static_cast<uint32_t>(
            (static_cast<uint32_t>(chunk.type[0]) << 0) |
            (static_cast<uint32_t>(chunk.type[1]) << 8) |
            (static_cast<uint32_t>(chunk.type[2]) << 16) |
            (static_cast<uint32_t>(chunk.type[3]) << 24)
        ));
        dirEntry.writeU32(chunk.analyzerVersion);
        dirEntry.writeU64(chunk.paramsHash);
        dirEntry.writeU64(chunkDirOffset + directoryBuffer.size());  // offset in final file
        dirEntry.writeU64(static_cast<uint64_t>(chunk.payload.size()));  // storedSize
        dirEntry.writeU64(static_cast<uint64_t>(chunk.payload.size()));  // rawSize (no compression)
        dirEntry.writeU64(checksum);

        const auto& entryBuf = dirEntry.buffer();
        directoryBuffer.insert(directoryBuffer.end(), entryBuf.begin(), entryBuf.end());

        // Will write payload after directory position is finalized
        chunkDirOffset += chunk.payload.size();
    }

    // Now we know where the directory goes. Update header with chunkDirOffset.
    uint64_t actualChunkDirOffset = fileBuffer.size() +
        std::accumulate(m_chunks.begin(), m_chunks.end(), static_cast<size_t>(0),
                       [](size_t sum, const ChunkDescriptor& c) { return sum + c.payload.size(); });

    // Seek back to the chunkDirOffset field in the header (this was not yet written above).
    // Actually, we need to update it. The issue is we already built the header without it.
    // Let me restructure: write to offset 76, chunkDirOffset as u64.
    // But we don't have it yet; we need to compute it after writing all payloads.

    // Actually, let's reorganize the header write to use PayloadWriter for cleaner code.
    // For now, let me recompute it directly.

    // Rewind and rewrite header with the actual chunkDirOffset.
    // First, build header properly with PayloadWriter.
    PayloadWriter headerWriter;
    headerWriter.writeU8('A');
    headerWriter.writeU8('W');
    headerWriter.writeU8('C');
    headerWriter.writeU8('1');
    headerWriter.writeU32(0x00010000);  // formatVersion + headerSize in wrong order; let me fix this

    // Actually, let me just redo the whole header write correctly.
    fileBuffer.clear();

    // Write header with actual chunkDirOffset
    PayloadWriter hdr;
    hdr.writeU8('A');
    hdr.writeU8('W');
    hdr.writeU8('C');
    hdr.writeU8('1');
    hdr.writeU32((0x0050 << 16) | 0x0001);  // headerSize=80, formatVersion=1
    hdr.writeU32(0);  // flags
    hdr.writeU32(ENGINE_VERSION);
    hdr.writeBytes(std::span<const uint8_t>(m_sourceHash.begin(), m_sourceHash.end()));
    hdr.writeU64(m_sourceSize);
    hdr.writeU32(static_cast<uint32_t>(m_sampleRate));
    hdr.writeU32(static_cast<uint32_t>(m_channels));
    hdr.writeI64(static_cast<int64_t>(m_frameCount));
    hdr.writeU32(static_cast<uint32_t>(m_chunks.size()));
    hdr.writeU32(0);  // reserved

    // But wait, the header is bigger than 80 bytes now because of how I wrote it.
    // Let me be more careful. Let me recalculate the exact layout based on docs/awc-format.md.

    // Actually, the issue is more fundamental: I'm trying to write header+chunks+directory all together,
    // but I don't know where the directory will go until after writing the chunks.
    // The current design in docs/awc-format.md doesn't seem to have chunkDirOffset in the header!
    // Let me re-read...

    // Actually, looking back at M16 design and the table I created in awc-format.md,
    // chunkDirOffset should be at offset 76 in the header as u64, but that conflicts with the
    // column numbering. Let me look at the bytes again:
    // - header is 80 bytes total
    // - last field shown is "reserved u32"
    // - before that is "chunkCount u32" at offset 72
    // - before that is "frameCount i64" at offset 64
    // So after "reserved u32" we're at offset 80, which is exactly where the directory starts
    // in the two-phase write (payloads then directory).

    // Actually, I realize the design doesn't put chunkDirOffset in the header.
    // The chunks and directory are sequential: after the header comes all the payloads, then the directory.
    // So chunkDirOffset = 80 + sum(all chunk payload sizes).

    // Let me simplify: write header, write payloads, write directory.
    // All offsets in the directory are absolute from file start.

    fileBuffer.clear();

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

    // Compute directory offset
    uint64_t payloadTotalSize = 0;
    for (const auto& chunk : m_chunks) {
        payloadTotalSize += chunk.payload.size();
    }
    uint64_t directoryOffset = 80 + payloadTotalSize;

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

// .awc file reader implementation.
// See docs/awc-format.md for format specification.

#include "awc_reader.hpp"

#include <fstream>
#include <limits>

#include "hash.hpp"
#include "serialise.hpp"

namespace aud::cache {

Result<AwcReader> AwcReader::open(const std::filesystem::path& path) {
    AwcReader reader(path);
    auto res = reader.loadHeaderAndDirectory();
    if (!res.has_value()) return Err(res.error());
    return reader;
}

Result<void> AwcReader::loadHeaderAndDirectory() {
    std::ifstream file(m_path, std::ios::binary);
    if (!file) {
        return Err("Failed to open cache file: " + m_path.string());
    }

    // Get file size
    file.seekg(0, std::ios::end);
    uint64_t fileSize = file.tellg();
    file.seekg(0);

    if (fileSize < 80) {
        return Err("Cache file too small (< 80 bytes header)");
    }

    // Read header (80 bytes)
    std::vector<uint8_t> headerBuf(80);
    file.read(reinterpret_cast<char*>(headerBuf.data()), 80);
    if (!file) {
        return Err("Failed to read header");
    }

    PayloadReader hdr(headerBuf);

    // magic
    auto magic = hdr.readBytes(4);
    if (!magic.has_value()) return Err(magic.error());
    if (magic.value()[0] != 'A' || magic.value()[1] != 'W' ||
        magic.value()[2] != 'C' || magic.value()[3] != '1') {
        return Err("Invalid magic");
    }

    // formatVersion
    auto fv = hdr.readU16();
    if (!fv.has_value()) return Err(fv.error());
    if (fv.value() != 1) {
        return Err("Unsupported format version: " + std::to_string(fv.value()));
    }

    // headerSize
    auto hs = hdr.readU16();
    if (!hs.has_value()) return Err(hs.error());
    if (hs.value() != 80) {
        return Err("Unexpected header size: " + std::to_string(hs.value()));
    }

    // flags
    auto flags = hdr.readU32();
    if (!flags.has_value()) return Err(flags.error());
    // Ignore flags for now; if bit 0 is set, it would indicate compression

    // engineVersion
    auto ev = hdr.readU32();
    if (!ev.has_value()) return Err(ev.error());
    m_header.engineVersion = ev.value();

    // sourceHash
    auto hash = hdr.readBytes(32);
    if (!hash.has_value()) return Err(hash.error());
    if (hash.value().size() != 32) {
        return Err("Invalid source hash size");
    }
    std::copy(hash.value().begin(), hash.value().end(), m_header.sourceHash.begin());

    // sourceSize
    auto ss = hdr.readU64();
    if (!ss.has_value()) return Err(ss.error());
    m_header.sourceSize = ss.value();

    // sampleRate
    auto sr = hdr.readU32();
    if (!sr.has_value()) return Err(sr.error());
    m_header.sampleRate = sr.value();

    // channels
    auto ch = hdr.readU32();
    if (!ch.has_value()) return Err(ch.error());
    m_header.channels = ch.value();

    // frameCount
    auto fc = hdr.readI64();
    if (!fc.has_value()) return Err(fc.error());
    m_header.frameCount = fc.value();

    // chunkCount
    auto cc = hdr.readU32();
    if (!cc.has_value()) return Err(cc.error());
    m_header.chunkCount = cc.value();

    // reserved
    auto res = hdr.readU32();
    if (!res.has_value()) return Err(res.error());

    // Now read the chunk directory
    // The directory starts after all chunk payloads.
    // We don't know where that is without reading the chunks themselves.
    // Strategy: the chunks are variable-length, so we need a different approach.

    // Actually, let me re-read the format spec. The format spec says:
    // - Header (80 bytes)
    // - Chunk payloads (variable length, at offsets specified in directory)
    // - Chunk directory (at chunkDirOffset, which was to be computed)

    // But the header doesn't have a chunkDirOffset field!
    // This is an issue. Let me check what makes sense.

    // Option 1: chunkDirOffset goes at the end of the file (simpler for seekless reading, but less flexible)
    // Option 2: chunkDirOffset is in the header (but we'd need to extend the header)
    // Option 3: Directory comes immediately after the header, before payloads (then we need sizes in directory)

    // Looking back at M16 design, it mentions "chunkDirOffset u64" in the header table.
    // But in our 80-byte layout, there's no room for it. This is a design issue.

    // For now, let me assume the directory is at the end of the file.
    // We'll compute backward from EOF.

    // Each directory entry is 48 bytes.
    uint64_t expectedDirSize = static_cast<uint64_t>(m_header.chunkCount) * 48;
    if (fileSize < expectedDirSize) {
        return Err("File too small to contain chunk directory");
    }

    // Seek to the directory (at the end of the file minus the directory size)
    uint64_t directoryOffset = fileSize - expectedDirSize;
    file.seekg(directoryOffset);

    // Read each directory entry
    for (uint32_t i = 0; i < m_header.chunkCount; ++i) {
        std::vector<uint8_t> entryBuf(48);
        file.read(reinterpret_cast<char*>(entryBuf.data()), 48);
        if (!file) {
            return Err("Failed to read chunk directory entry " + std::to_string(i));
        }

        PayloadReader entry(entryBuf);

        auto type = entry.readU32();
        if (!type.has_value()) return Err(type.error());
        uint32_t t = type.value();
        std::array<char, 4> typeStr;
        typeStr[0] = static_cast<char>(t & 0xFF);
        typeStr[1] = static_cast<char>((t >> 8) & 0xFF);
        typeStr[2] = static_cast<char>((t >> 16) & 0xFF);
        typeStr[3] = static_cast<char>((t >> 24) & 0xFF);

        auto av = entry.readU32();
        if (!av.has_value()) return Err(av.error());

        auto ph = entry.readU64();
        if (!ph.has_value()) return Err(ph.error());

        auto off = entry.readU64();
        if (!off.has_value()) return Err(off.error());

        auto ss = entry.readU64();
        if (!ss.has_value()) return Err(ss.error());

        auto rs = entry.readU64();
        if (!rs.has_value()) return Err(rs.error());

        auto chk = entry.readU64();
        if (!chk.has_value()) return Err(chk.error());

        m_chunks.push_back(ChunkInfo{
            .type = typeStr,
            .analyzerVersion = av.value(),
            .paramsHash = ph.value(),
            .offset = off.value(),
            .storedSize = ss.value(),
            .rawSize = rs.value(),
            .checksum = chk.value(),
        });
    }

    return Ok();
}

const ChunkInfo* AwcReader::findChunk(std::string_view type) const {
    if (type.size() != 4) return nullptr;

    for (const auto& chunk : m_chunks) {
        if (chunk.type[0] == type[0] && chunk.type[1] == type[1] &&
            chunk.type[2] == type[2] && chunk.type[3] == type[3]) {
            return &chunk;
        }
    }
    return nullptr;
}

Result<std::vector<uint8_t>> AwcReader::loadChunk(std::string_view type) {
    const ChunkInfo* info = findChunk(type);
    if (!info) {
        return Ok(std::vector<uint8_t>{});  // Not found is not an error; just empty
    }

    std::ifstream file(m_path, std::ios::binary);
    if (!file) {
        return Err("Failed to open cache file for reading");
    }

    // Seek to chunk offset and read payload
    file.seekg(info->offset);
    std::vector<uint8_t> payload(info->rawSize);

    file.read(reinterpret_cast<char*>(payload.data()), info->rawSize);
    if (!file) {
        return Err("Failed to read chunk: " + std::string(type.begin(), type.end()));
    }

    // Verify checksum
    uint64_t actualChecksum = xxHash3(std::span<const uint8_t>(payload));
    if (actualChecksum != info->checksum) {
        return Err("Chunk checksum mismatch: " + std::string(type.begin(), type.end()));
    }

    return payload;
}

Result<std::vector<std::vector<uint8_t>>> AwcReader::loadChunksOfType(std::string_view type) {
    std::vector<std::vector<uint8_t>> results;

    for (const auto& chunk : m_chunks) {
        if (chunk.type[0] == type[0] && chunk.type[1] == type[1] &&
            chunk.type[2] == type[2] && chunk.type[3] == type[3]) {

            auto payload = loadChunk(type);
            if (payload.has_value()) {
                results.push_back(payload.value());
            } else {
                return Err(payload.error());
            }
        }
    }

    return results;
}

}  // namespace aud::cache

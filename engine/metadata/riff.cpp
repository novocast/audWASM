#include "riff.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>

#include "id3v2.hpp"

namespace aud::metadata {

namespace {

std::uint32_t readU32Le(std::span<const std::byte> b, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(b[offset]) | (static_cast<std::uint32_t>(b[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(b[offset + 2]) << 16) | (static_cast<std::uint32_t>(b[offset + 3]) << 24);
}

std::int16_t readS16Le(std::span<const std::byte> b, std::size_t offset) noexcept {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(b[offset]) |
                                      (static_cast<std::uint16_t>(b[offset + 1]) << 8));
}

struct RiffChunk {
    std::string                type;
    std::span<const std::byte> body;
};

// Direct children of a RIFF list body (the 4 bytes after "RIFF"+size, or after a LIST's own
// 4-byte form-type). Chunk sizes are LE and padded to an even boundary (the pad byte is not part
// of the declared size and is skipped between chunks, not within one).
std::size_t forEachRiffChunk(std::span<const std::byte> bytes, const std::function<void(const RiffChunk&)>& visit) {
    std::size_t cursor  = 0;
    std::size_t visited = 0;
    while (cursor + 8 <= bytes.size()) {
        const std::string   id(reinterpret_cast<const char*>(bytes.data() + cursor), 4);
        const std::uint32_t size = readU32Le(bytes, cursor + 4);
        if (cursor + 8 + size > bytes.size()) break;  // truncated — stop safely

        visit(RiffChunk{id, bytes.subspan(cursor + 8, size)});
        ++visited;

        std::size_t advance = 8 + size;
        if (size % 2 != 0) ++advance;  // pad byte
        if (cursor + advance > bytes.size()) break;
        cursor += advance;
    }
    return visited;
}

// Trims a NUL/space-padded fixed-width field to a plain string (RIFF INFO/bext text fields are
// conventionally ASCII/Latin-1).
std::string trimField(std::span<const std::byte> field) {
    std::size_t len = field.size();
    while (len > 0) {
        const auto c = static_cast<std::uint8_t>(field[len - 1]);
        if (c != 0 && c != ' ') break;
        --len;
    }
    return std::string(reinterpret_cast<const char*>(field.data()), len);
}

std::string hexEncode(std::span<const std::byte> bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string        out;
    out.reserve(bytes.size() * 2);
    for (std::byte b : bytes) {
        const auto v = static_cast<std::uint8_t>(b);
        out.push_back(kHex[v >> 4]);
        out.push_back(kHex[v & 0x0F]);
    }
    return out;
}

void parseInfoList(std::span<const std::byte> listBody, TagSet& tags) {
    if (listBody.size() < 4) return;
    if (std::memcmp(listBody.data(), "INFO", 4) != 0) return;

    forEachRiffChunk(listBody.subspan(4), [&](const RiffChunk& chunk) {
        const std::string text = trimField(chunk.body);
        if (text.empty()) return;

        if (chunk.type == "INAM") { if (!tags.title) tags.title = text; }
        else if (chunk.type == "IART") { if (!tags.artist) tags.artist = text; }
        else if (chunk.type == "IPRD") { if (!tags.album) tags.album = text; }
        else if (chunk.type == "ICRD") {
            if (!tags.date) tags.date = text;
            if (!tags.year && text.size() >= 4 &&
                std::all_of(text.begin(), text.begin() + 4,
                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
                tags.year = static_cast<std::uint32_t>(std::strtoul(text.substr(0, 4).c_str(), nullptr, 10));
            }
        } else if (chunk.type == "IGNR") { if (!tags.genre) tags.genre = text; }
        else if (chunk.type == "ICMT") { if (!tags.comment) tags.comment = text; }
        else if (chunk.type == "ISFT") { if (!tags.encoderSettings) tags.encoderSettings = text; }
        else if (chunk.type == "ICOP") { if (!tags.copyright) tags.copyright = text; }
        else tags.unmapped.emplace_back(chunk.type, MetadataValue{text, "riff", chunk.type});
    });
}

void parseBext(std::span<const std::byte> body, TagSet& tags) {
    constexpr std::size_t kFixedSize = 602;
    if (body.size() < kFixedSize) {
        tags.diagnostics.push_back({Severity::Warning, "riff: bext chunk shorter than the fixed BWF layout; parsing what's present"});
    }
    const std::size_t avail = std::min(body.size(), kFixedSize);
    if (avail < 350) return;  // not even enough for description/originator/date/time/timeref/version

    BroadcastInfo info;
    info.present             = true;
    info.description         = trimField(body.subspan(0, 256));
    info.originator          = trimField(body.subspan(256, 32));
    info.originatorReference = trimField(body.subspan(288, 32));
    info.originationDate     = trimField(body.subspan(320, 10));
    info.originationTime     = trimField(body.subspan(330, 8));
    const std::uint32_t timeRefLow  = readU32Le(body, 338);
    const std::uint32_t timeRefHigh = readU32Le(body, 342);
    info.timeReference               = (static_cast<std::uint64_t>(timeRefHigh) << 32) | timeRefLow;
    info.version                     = static_cast<std::uint16_t>(readU32Le(body, 346) & 0xFFFF);

    if (avail >= 412) info.umid = hexEncode(body.subspan(412 - 64, 64));
    if (info.version >= 1 && avail >= 422) {
        info.loudnessValueLufs = static_cast<double>(readS16Le(body, 412)) / 100.0;
        info.loudnessRangeLu   = static_cast<double>(readS16Le(body, 414)) / 100.0;
        info.maxTruePeakDbtp    = static_cast<double>(readS16Le(body, 416)) / 100.0;
        info.maxMomentaryLufs   = static_cast<double>(readS16Le(body, 418)) / 100.0;
        info.maxShortTermLufs   = static_cast<double>(readS16Le(body, 420)) / 100.0;
    }
    if (body.size() > kFixedSize) {
        info.codingHistory = trimField(body.subspan(kFixedSize));
    }

    tags.broadcast = std::move(info);
}

}  // namespace

RiffParseResult parseRiff(std::span<const std::byte> bytes) {
    RiffParseResult result;
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return result;
    }

    result.present            = true;
    result.tags.sourceFormat = "riff";

    std::uint32_t sampleRate = 0;
    std::map<std::uint32_t, double> cueOffsetsSeconds;   // cue point id -> time
    std::map<std::uint32_t, std::string> cueLabels;       // cue point id -> label text

    forEachRiffChunk(bytes.subspan(12), [&](const RiffChunk& chunk) {
        if (chunk.type == "fmt " && chunk.body.size() >= 8) {
            sampleRate = readU32Le(chunk.body, 4);
        } else if (chunk.type == "LIST") {
            parseInfoList(chunk.body, result.tags);
            if (chunk.body.size() >= 4 && std::memcmp(chunk.body.data(), "adtl", 4) == 0) {
                forEachRiffChunk(chunk.body.subspan(4), [&](const RiffChunk& sub) {
                    if (sub.type == "labl" && sub.body.size() >= 4) {
                        const std::uint32_t id = readU32Le(sub.body, 0);
                        cueLabels[id]            = trimField(sub.body.subspan(4));
                    }
                });
            }
        } else if (chunk.type == "bext") {
            parseBext(chunk.body, result.tags);
        } else if (chunk.type == "id3 " || chunk.type == "ID3 ") {
            Id3v2ParseResult id3 = parseId3v2(chunk.body);
            if (id3.present) appendTagSet(result.tags, std::move(id3.tags));
        } else if (chunk.type == "iXML") {
            constexpr std::size_t kCap = 16384;
            const std::size_t     len  = std::min(chunk.body.size(), kCap);
            std::string            text(reinterpret_cast<const char*>(chunk.body.data()), len);
            if (chunk.body.size() > kCap) {
                result.tags.diagnostics.push_back({Severity::Info, "riff: iXML chunk truncated to 16KB for display"});
            }
            result.tags.unmapped.emplace_back("iXML", MetadataValue{text, "riff", "iXML"});
        } else if (chunk.type == "cue " && chunk.body.size() >= 4) {
            const std::uint32_t count = readU32Le(chunk.body, 0);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::size_t offset = 4 + static_cast<std::size_t>(i) * 24;
                if (offset + 24 > chunk.body.size()) break;
                const std::uint32_t id            = readU32Le(chunk.body, offset);
                const std::uint32_t sampleOffset = readU32Le(chunk.body, offset + 20);
                cueOffsetsSeconds[id]              = static_cast<double>(sampleOffset);  // divided by sampleRate below
            }
        }
    });

    if (!cueOffsetsSeconds.empty()) {
        for (auto& [id, sampleOffset] : cueOffsetsSeconds) {
            CuePoint cp;
            cp.timeSeconds  = sampleRate > 0 ? sampleOffset / static_cast<double>(sampleRate) : 0.0;
            auto label       = cueLabels.find(id);
            cp.label         = label != cueLabels.end() ? label->second : ("Cue " + std::to_string(id));
            cp.sourceFormat = "riff.cue";
            result.tags.cuePoints.push_back(std::move(cp));
        }
    }

    return result;
}

}  // namespace aud::metadata

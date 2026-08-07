#include "apev2.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "replaygain.hpp"

namespace aud::metadata {

namespace {

constexpr std::size_t kFooterSize = 32;

std::uint32_t readU32Le(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool isApeMagic(std::span<const std::byte> bytes, std::size_t offset) {
    static constexpr char kMagic[] = "APETAGEX";
    if (offset + 8 > bytes.size()) return false;
    return std::memcmp(bytes.data() + offset, kMagic, 8) == 0;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

void assignMapped(const std::string& upperKey, const std::string& value, TagSet& tags) {
    if (upperKey == "TITLE") { if (!tags.title) tags.title = value; }
    else if (upperKey == "ARTIST") { if (!tags.artist) tags.artist = value; }
    else if (upperKey == "ALBUM ARTIST" || upperKey == "ALBUMARTIST") { if (!tags.albumArtist) tags.albumArtist = value; }
    else if (upperKey == "ALBUM") { if (!tags.album) tags.album = value; }
    else if (upperKey == "GENRE") { if (!tags.genre) tags.genre = value; }
    else if (upperKey == "COMPOSER") { if (!tags.composer) tags.composer = value; }
    else if (upperKey == "COMMENT") { if (!tags.comment) tags.comment = value; }
    else if (upperKey == "PUBLISHER" || upperKey == "LABEL") { if (!tags.publisher) tags.publisher = value; }
    else if (upperKey == "COPYRIGHT") { if (!tags.copyright) tags.copyright = value; }
    else if (upperKey == "ISRC") { if (!tags.isrc) tags.isrc = value; }
    else if (upperKey == "MUSICBRAINZ_TRACKID") { if (!tags.musicBrainzTrackId) tags.musicBrainzTrackId = value; }
    else if (upperKey == "MUSICBRAINZ_ALBUMID") { if (!tags.musicBrainzAlbumId) tags.musicBrainzAlbumId = value; }
    else if (upperKey == "YEAR") {
        if (!value.empty() && std::isdigit(static_cast<unsigned char>(value[0])) && !tags.year) {
            tags.year = static_cast<std::uint32_t>(std::strtoul(value.substr(0, 4).c_str(), nullptr, 10));
        }
        if (!tags.date) tags.date = value;
    } else if (upperKey == "TRACK") {
        const std::size_t slash = value.find('/');
        const std::string numPart = slash == std::string::npos ? value : value.substr(0, slash);
        if (!numPart.empty() && !tags.trackNumber) {
            tags.trackNumber = static_cast<std::uint32_t>(std::strtoul(numPart.c_str(), nullptr, 10));
        }
        if (slash != std::string::npos && !tags.trackTotal) {
            tags.trackTotal = static_cast<std::uint32_t>(std::strtoul(value.substr(slash + 1).c_str(), nullptr, 10));
        }
    } else if (upperKey == "REPLAYGAIN_TRACK_GAIN") {
        ReplayGainSource src;
        src.origin      = "apev2";
        src.trackGainDb = parseReplayGainDb(value);
        tags.replayGainSources.push_back(src);
    } else if (upperKey == "REPLAYGAIN_TRACK_PEAK") {
        ReplayGainSource src;
        src.origin     = "apev2";
        src.trackPeak  = parseReplayGainPeak(value);
        tags.replayGainSources.push_back(src);
    } else {
        tags.unmapped.emplace_back(upperKey, MetadataValue{value, "apev2", upperKey});
    }
}

}  // namespace

Apev2ParseResult parseApev2(std::span<const std::byte> bytes) {
    Apev2ParseResult result;
    if (bytes.size() < kFooterSize) return result;

    std::size_t footerStart = bytes.size() - kFooterSize;
    if (!isApeMagic(bytes, footerStart)) {
        // Try just before a trailing ID3v1 tag.
        if (bytes.size() < kFooterSize + 128) return result;
        footerStart = bytes.size() - 128 - kFooterSize;
        if (!isApeMagic(bytes, footerStart)) return result;
    }

    const std::uint32_t flags   = readU32Le(bytes, footerStart + 20);
    const bool           isHeaderNotFooter = (flags & 0x20000000u) != 0;
    if (isHeaderNotFooter) return result;  // found a header where we expected a footer — bail out safely

    const std::uint32_t tagSize   = readU32Le(bytes, footerStart + 12);
    const std::uint32_t itemCount = readU32Le(bytes, footerStart + 16);

    if (tagSize < kFooterSize || tagSize > footerStart + kFooterSize) {
        result.tags.diagnostics.push_back({Severity::Warning, "apev2: implausible tag size, skipped"});
        return result;
    }
    const std::size_t itemsSize  = tagSize - kFooterSize;
    if (itemsSize > footerStart) {
        result.tags.diagnostics.push_back({Severity::Warning, "apev2: tag size overruns start of file, skipped"});
        return result;
    }
    const std::size_t itemsStart = footerStart - itemsSize;

    result.present            = true;
    result.tags.sourceFormat = "apev2";

    std::span<const std::byte> items = bytes.subspan(itemsStart, itemsSize);
    std::size_t                 offset = 0;
    std::uint32_t                parsed = 0;

    while (offset + 8 <= items.size() && parsed < itemCount) {
        const std::uint32_t valueSize = readU32Le(items, offset);
        // itemFlags at items[offset+4..8): bits 1-2 are the value type; only text (0) is decoded.
        const std::uint32_t itemFlags = readU32Le(items, offset + 4);
        const std::uint32_t valueType = (itemFlags >> 1) & 0x3;
        offset += 8;

        const std::size_t keyStart = offset;
        std::size_t         keyEnd   = keyStart;
        while (keyEnd < items.size() && items[keyEnd] != std::byte{0}) ++keyEnd;
        if (keyEnd >= items.size()) break;  // no terminator found — truncated, stop safely
        const std::string key(reinterpret_cast<const char*>(items.data() + keyStart), keyEnd - keyStart);
        offset = keyEnd + 1;

        if (offset + valueSize > items.size()) {
            result.tags.diagnostics.push_back(
                {Severity::Warning, "apev2: item '" + key + "' declares a size that overruns the tag; stopping"});
            break;
        }

        if (valueType == 0 && !key.empty()) {  // UTF-8 text; multiple values are 0x00-separated
            std::string value(reinterpret_cast<const char*>(items.data() + offset), valueSize);
            const std::size_t nul = value.find('\0');
            assignMapped(upper(key), nul == std::string::npos ? value : value.substr(0, nul), result.tags);
        } else if (!key.empty()) {
            result.tags.unmapped.emplace_back(
                key, MetadataValue{"<" + std::to_string(valueSize) + " binary bytes>", "apev2", key});
        }

        offset += valueSize;
        ++parsed;
    }

    return result;
}

}  // namespace aud::metadata

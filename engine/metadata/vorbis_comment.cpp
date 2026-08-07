#include "vorbis_comment.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "cue.hpp"
#include "lyrics.hpp"
#include "pictures.hpp"
#include "replaygain.hpp"

namespace aud::metadata {

namespace {

std::uint32_t readU32Le(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint32_t readU32Be(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

// Shared by the native FLAC PICTURE metadata block and the base64-decoded METADATA_BLOCK_PICTURE
// Vorbis comment (same fixed layout either way): type(4BE), mimeLen(4BE)+mime, descLen(4BE)+desc,
// width/height/depth/colors (4BE each, unused here), dataLen(4BE)+data. Every length is checked
// against what's left of `block` before use.
bool parseFlacPictureBlockBytes(std::span<const std::byte> block, const std::string& sourceFormat, Picture& out,
                                 std::vector<Diagnostic>& diagnostics) {
    if (block.size() < 32) return false;

    const auto  pictureType = static_cast<PictureType>(readU32Be(block, 0));
    std::size_t offset       = 4;

    const std::uint32_t mimeLen = readU32Be(block, offset);
    offset += 4;
    if (offset + mimeLen > block.size()) return false;
    const std::string mimeType(reinterpret_cast<const char*>(block.data() + offset), mimeLen);
    offset += mimeLen;

    if (offset + 4 > block.size()) return false;
    const std::uint32_t descLen = readU32Be(block, offset);
    offset += 4;
    if (offset + descLen > block.size()) return false;
    const std::string description(reinterpret_cast<const char*>(block.data() + offset), descLen);
    offset += descLen;

    if (offset + 16 > block.size()) return false;  // width, height, depth, colors — unused, still bounds-checked
    offset += 16;

    if (offset + 4 > block.size()) return false;
    const std::uint32_t dataLen = readU32Be(block, offset);
    offset += 4;
    if (offset + dataLen > block.size()) return false;

    return extractPicture(block.subspan(offset, dataLen), mimeType, pictureType, description, sourceFormat, out,
                           diagnostics);
}

// Minimal, defensive base64 decoder for METADATA_BLOCK_PICTURE. Invalid characters/padding just
// stop decoding early rather than reading past a lookup table or producing garbage silently.
std::vector<std::byte> base64Decode(std::string_view text) {
    auto valueOf = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);

    int          buffer     = 0;
    int          bitsInBuf  = 0;
    for (char c : text) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
        const int v = valueOf(c);
        if (v < 0) break;  // malformed input — stop rather than misinterpret
        buffer    = (buffer << 6) | v;
        bitsInBuf += 6;
        if (bitsInBuf >= 8) {
            bitsInBuf -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bitsInBuf) & 0xFF));
        }
    }
    return out;
}

void assignVorbisKey(const std::string& upperKey, const std::string& value, const std::string& sourceFormat,
                       TagSet& tags) {
    if (upperKey == "TITLE") { if (!tags.title) tags.title = value; }
    else if (upperKey == "ARTIST") { if (!tags.artist) tags.artist = value; }
    else if (upperKey == "ALBUMARTIST" || upperKey == "ALBUM ARTIST") { if (!tags.albumArtist) tags.albumArtist = value; }
    else if (upperKey == "ALBUM") { if (!tags.album) tags.album = value; }
    else if (upperKey == "GENRE") { if (!tags.genre) tags.genre = value; }
    else if (upperKey == "COMPOSER") { if (!tags.composer) tags.composer = value; }
    else if (upperKey == "COMMENT" || upperKey == "DESCRIPTION") { if (!tags.comment) tags.comment = value; }
    else if (upperKey == "ORGANIZATION" || upperKey == "PUBLISHER" || upperKey == "LABEL") {
        if (!tags.publisher) tags.publisher = value;
    } else if (upperKey == "COPYRIGHT") { if (!tags.copyright) tags.copyright = value; }
    else if (upperKey == "ENCODED-BY" || upperKey == "ENCODEDBY") { if (!tags.encodedBy) tags.encodedBy = value; }
    else if (upperKey == "ENCODER") { if (!tags.encoderSettings) tags.encoderSettings = value; }
    else if (upperKey == "ISRC") { if (!tags.isrc) tags.isrc = value; }
    else if (upperKey == "UPC" || upperKey == "BARCODE") { if (!tags.upc) tags.upc = value; }
    else if (upperKey == "CATALOGNUMBER") { if (!tags.catalogNumber) tags.catalogNumber = value; }
    else if (upperKey == "MUSICBRAINZ_TRACKID") { if (!tags.musicBrainzTrackId) tags.musicBrainzTrackId = value; }
    else if (upperKey == "MUSICBRAINZ_ALBUMID") { if (!tags.musicBrainzAlbumId) tags.musicBrainzAlbumId = value; }
    else if (upperKey == "DATE") {
        if (!tags.date) tags.date = value;
        if (!tags.year && value.size() >= 4 &&
            std::all_of(value.begin(), value.begin() + 4, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
            tags.year = static_cast<std::uint32_t>(std::strtoul(value.substr(0, 4).c_str(), nullptr, 10));
        }
    } else if (upperKey == "BPM" || upperKey == "TEMPO") {
        if (!tags.bpm && !value.empty() && std::isdigit(static_cast<unsigned char>(value[0]))) {
            tags.bpm = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        }
    } else if (upperKey == "TRACKNUMBER") {
        const std::size_t slash = value.find('/');
        const std::string num   = slash == std::string::npos ? value : value.substr(0, slash);
        if (!num.empty() && !tags.trackNumber) tags.trackNumber = static_cast<std::uint32_t>(std::strtoul(num.c_str(), nullptr, 10));
        if (slash != std::string::npos && !tags.trackTotal) {
            tags.trackTotal = static_cast<std::uint32_t>(std::strtoul(value.substr(slash + 1).c_str(), nullptr, 10));
        }
    } else if (upperKey == "TRACKTOTAL" || upperKey == "TOTALTRACKS") {
        if (!tags.trackTotal && !value.empty()) tags.trackTotal = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (upperKey == "DISCNUMBER") {
        const std::size_t slash = value.find('/');
        const std::string num   = slash == std::string::npos ? value : value.substr(0, slash);
        if (!num.empty() && !tags.discNumber) tags.discNumber = static_cast<std::uint32_t>(std::strtoul(num.c_str(), nullptr, 10));
        if (slash != std::string::npos && !tags.discTotal) {
            tags.discTotal = static_cast<std::uint32_t>(std::strtoul(value.substr(slash + 1).c_str(), nullptr, 10));
        }
    } else if (upperKey == "DISCTOTAL" || upperKey == "TOTALDISCS") {
        if (!tags.discTotal && !value.empty()) tags.discTotal = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (upperKey == "REPLAYGAIN_TRACK_GAIN" || upperKey == "REPLAYGAIN_ALBUM_GAIN") {
        ReplayGainSource src;
        src.origin = "vorbis";
        if (upperKey == "REPLAYGAIN_TRACK_GAIN") src.trackGainDb = parseReplayGainDb(value);
        else src.albumGainDb = parseReplayGainDb(value);
        tags.replayGainSources.push_back(src);
    } else if (upperKey == "REPLAYGAIN_TRACK_PEAK" || upperKey == "REPLAYGAIN_ALBUM_PEAK") {
        ReplayGainSource src;
        src.origin = "vorbis";
        if (upperKey == "REPLAYGAIN_TRACK_PEAK") src.trackPeak = parseReplayGainPeak(value);
        else src.albumPeak = parseReplayGainPeak(value);
        tags.replayGainSources.push_back(src);
    } else if (upperKey == "LYRICS" || upperKey == "UNSYNCEDLYRICS" || upperKey == "SYNCEDLYRICS") {
        tags.lyrics.push_back(makeLyricsFromUnsyncedText(value, "", "", sourceFormat));
    } else if (upperKey == "CUESHEET") {
        for (auto& cp : parseExternalCueSheet(value)) tags.cuePoints.push_back(std::move(cp));
    } else if (upperKey == "METADATA_BLOCK_PICTURE") {
        std::vector<std::byte> decoded = base64Decode(value);
        Picture                 pic;
        if (parseFlacPictureBlockBytes(decoded, sourceFormat, pic, tags.diagnostics)) {
            tags.pictures.push_back(std::move(pic));
        }
    } else {
        tags.unmapped.emplace_back(upperKey, MetadataValue{value, sourceFormat, upperKey});
    }
}

}  // namespace

TagSet parseVorbisCommentPayload(std::span<const std::byte> payload, const std::string& sourceFormat) {
    TagSet tags;
    tags.sourceFormat = sourceFormat;

    if (payload.size() < 8) {
        tags.diagnostics.push_back({Severity::Warning, sourceFormat + ": Vorbis comment block truncated before vendor string"});
        return tags;
    }

    std::size_t offset = 0;
    const std::uint32_t vendorLen = readU32Le(payload, offset);
    offset += 4;
    if (offset + vendorLen + 4 > payload.size()) {
        tags.diagnostics.push_back({Severity::Warning, sourceFormat + ": Vorbis comment vendor string overruns block"});
        return tags;
    }
    offset += vendorLen;  // vendor string itself isn't surfaced as a field

    const std::uint32_t commentCount = readU32Le(payload, offset);
    offset += 4;

    for (std::uint32_t i = 0; i < commentCount; ++i) {
        if (offset + 4 > payload.size()) {
            tags.diagnostics.push_back({Severity::Warning, sourceFormat + ": comment list truncated; stopping"});
            break;
        }
        const std::uint32_t len = readU32Le(payload, offset);
        offset += 4;
        if (offset + len > payload.size()) {
            tags.diagnostics.push_back({Severity::Warning, sourceFormat + ": a comment entry overruns the block; stopping"});
            break;
        }
        const std::string entry(reinterpret_cast<const char*>(payload.data() + offset), len);
        offset += len;

        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;  // not KEY=VALUE — ignore, per the Vorbis comment spec

        assignVorbisKey(upper(entry.substr(0, eq)), entry.substr(eq + 1), sourceFormat, tags);
    }

    return tags;
}

FlacParseResult parseFlacMetadataBlocks(std::span<const std::byte> bytes) {
    FlacParseResult result;
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "fLaC", 4) != 0) return result;

    result.present = true;
    result.tags.sourceFormat = "vorbis";

    std::size_t cursor = 4;
    bool         lastBlock = false;
    while (!lastBlock && cursor + 4 <= bytes.size()) {
        const auto  headerByte = static_cast<std::uint8_t>(bytes[cursor]);
        lastBlock                = (headerByte & 0x80) != 0;
        const std::uint8_t blockType = headerByte & 0x7F;
        const std::uint32_t blockLen  = (static_cast<std::uint32_t>(bytes[cursor + 1]) << 16) |
                                        (static_cast<std::uint32_t>(bytes[cursor + 2]) << 8) |
                                        static_cast<std::uint32_t>(bytes[cursor + 3]);
        cursor += 4;
        if (cursor + blockLen > bytes.size()) {
            result.tags.diagnostics.push_back({Severity::Warning, "flac: metadata block overruns file; stopping"});
            break;
        }
        std::span<const std::byte> body = bytes.subspan(cursor, blockLen);

        if (blockType == 0 && body.size() >= 18) {  // STREAMINFO
            result.sampleRate = (static_cast<std::uint32_t>(body[10]) << 12) |
                                (static_cast<std::uint32_t>(body[11]) << 4) |
                                (static_cast<std::uint32_t>(body[12]) >> 4);
        } else if (blockType == 4) {  // VORBIS_COMMENT
            // A conformant file has exactly one VORBIS_COMMENT block, but merge defensively
            // (first-non-empty-wins, all collections concatenated) rather than assume.
            appendTagSet(result.tags, parseVorbisCommentPayload(body, "vorbis"));
        } else if (blockType == 6) {  // PICTURE
            Picture pic;
            if (parseFlacPictureBlockBytes(body, "flac", pic, result.tags.diagnostics)) {
                result.tags.pictures.push_back(std::move(pic));
            }
        } else if (blockType == 5) {  // CUESHEET
            for (auto& cp : parseFlacCueSheet(body, result.sampleRate)) result.tags.cuePoints.push_back(std::move(cp));
        }

        cursor += blockLen;
    }

    return result;
}

OggParseResult parseOggVorbisComment(std::span<const std::byte> bytes) {
    OggParseResult result;
    if (bytes.size() < 27 || std::memcmp(bytes.data(), "OggS", 4) != 0) return result;

    std::vector<std::vector<std::byte>> packets;
    std::vector<std::byte>               current;

    std::size_t cursor = 0;
    while (cursor + 27 <= bytes.size() && packets.size() < 2) {
        if (std::memcmp(bytes.data() + cursor, "OggS", 4) != 0) break;  // lost sync — stop, don't guess
        const std::uint8_t segmentCount = static_cast<std::uint8_t>(bytes[cursor + 26]);
        if (cursor + 27 + segmentCount > bytes.size()) break;
        std::span<const std::byte> segmentTable = bytes.subspan(cursor + 27, segmentCount);

        std::size_t dataCursor = cursor + 27 + segmentCount;
        for (std::size_t i = 0; i < segmentTable.size(); ++i) {
            const auto segLen = static_cast<std::uint8_t>(segmentTable[i]);
            if (dataCursor + segLen > bytes.size()) { dataCursor = bytes.size(); break; }
            current.insert(current.end(), bytes.begin() + static_cast<std::ptrdiff_t>(dataCursor),
                            bytes.begin() + static_cast<std::ptrdiff_t>(dataCursor + segLen));
            dataCursor += segLen;
            if (segLen < 255) {
                packets.push_back(std::move(current));
                current.clear();
                if (packets.size() >= 2) break;
            }
        }

        cursor = dataCursor;
    }

    if (packets.size() < 2) return result;

    const auto& commentPacket = packets[1];
    static constexpr char kPrefix[] = "\x03vorbis";
    if (commentPacket.size() < 7 || std::memcmp(commentPacket.data(), kPrefix, 7) != 0) return result;

    result.present = true;
    result.tags     = parseVorbisCommentPayload(std::span<const std::byte>(commentPacket).subspan(7), "vorbis");
    return result;
}

}  // namespace aud::metadata

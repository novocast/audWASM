#include "id3v2.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "lyrics.hpp"
#include "pictures.hpp"
#include "replaygain.hpp"
#include "text_encoding.hpp"

namespace aud::metadata {

namespace {

std::uint32_t syncsafeToUint32(std::span<const std::byte> b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 21) | (static_cast<std::uint32_t>(b[1]) << 14) |
           (static_cast<std::uint32_t>(b[2]) << 7) | static_cast<std::uint32_t>(b[3]);
}

std::uint32_t plainBeToUint32(std::span<const std::byte> b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) |
           (static_cast<std::uint32_t>(b[2]) << 8) | static_cast<std::uint32_t>(b[3]);
}

std::uint32_t plainBeToUint24(std::span<const std::byte> b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 16) | (static_cast<std::uint32_t>(b[1]) << 8) |
           static_cast<std::uint32_t>(b[2]);
}

// Undoes ID3v2 unsynchronisation: every $FF $00 pair in the raw stream had its $00 inserted purely
// to break false MPEG frame syncs, so decoding is just "drop the $00 half of every $FF $00 pair".
std::vector<std::byte> removeUnsynchronisation(std::span<const std::byte> bytes) {
    std::vector<std::byte> out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out.push_back(bytes[i]);
        if (bytes[i] == std::byte{0xFF} && i + 1 < bytes.size() && bytes[i + 1] == std::byte{0x00}) {
            ++i;  // drop the inserted 0x00
        }
    }
    return out;
}

TextEncoding textEncodingFromByte(std::uint8_t b) noexcept {
    switch (b) {
        case 1:  return TextEncoding::Utf16Bom;
        case 2:  return TextEncoding::Utf16Be;
        case 3:  return TextEncoding::Utf8;
        default: return TextEncoding::Latin1;
    }
}

// Reads a single NUL-terminated string encoded per `encoding`, starting at `bytes[offset]`.
// Returns the decoded UTF-8 text and advances `offset` past the terminator (1 byte for
// Latin1/UTF-8, 2 bytes for either UTF-16 variant). If no terminator is found before the end of
// `bytes`, consumes the remainder (defensive: a missing terminator is malformed input, not a
// license to read out of bounds).
std::string readTerminatedString(std::span<const std::byte> bytes, std::size_t& offset, TextEncoding encoding) {
    const bool wide = (encoding == TextEncoding::Utf16Bom || encoding == TextEncoding::Utf16Be);
    std::size_t i    = offset;

    if (wide) {
        while (i + 1 < bytes.size() && !(bytes[i] == std::byte{0} && bytes[i + 1] == std::byte{0})) i += 2;
        const std::size_t end = std::min(i, bytes.size());
        std::string        text = decodeToUtf8(bytes.subspan(offset, end - offset), encoding);
        offset                  = std::min(end + 2, bytes.size());
        return text;
    }

    while (i < bytes.size() && bytes[i] != std::byte{0}) ++i;
    std::string text = decodeToUtf8(bytes.subspan(offset, i - offset), TextEncoding::Latin1);
    // Latin1 decode is safe for UTF-8 payloads here too (readTerminatedString's caller already knows
    // the real encoding when it isn't Latin1/UTF-8-ambiguous); redo with the real encoding for UTF-8:
    if (encoding == TextEncoding::Utf8) text = decodeToUtf8(bytes.subspan(offset, i - offset), TextEncoding::Utf8);
    offset = std::min(i + 1, bytes.size());
    return text;
}

std::string canonicalFrameId(int majorVersion, const std::string& id) {
    if (majorVersion != 2 || id.size() != 3) return id;
    static const std::pair<const char*, const char*> kMap[] = {
        {"TT2", "TIT2"}, {"TP1", "TPE1"}, {"TP2", "TPE2"}, {"TAL", "TALB"}, {"TCO", "TCON"},
        {"TCM", "TCOM"}, {"TPA", "TPOS"}, {"TRK", "TRCK"}, {"TYE", "TYER"}, {"TDA", "TDAT"},
        {"TIM", "TIME"}, {"TPB", "TPUB"}, {"TCR", "TCOP"}, {"TEN", "TENC"}, {"TSS", "TSSE"},
        {"TBP", "TBPM"}, {"TRC", "TSRC"}, {"TXX", "TXXX"}, {"COM", "COMM"}, {"ULT", "USLT"},
        {"SLT", "SYLT"}, {"PIC", "APIC"}, {"WXX", "WXXX"},
    };
    for (const auto& [from, to] : kMap) {
        if (id == from) return to;
    }
    return id;
}

// Splits "N", "N/M" (TPOS/TRCK convention) into (number, total).
bool isAsciiDigit(char c) noexcept { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

void parseNumberSlashTotal(const std::string& text, std::optional<std::uint32_t>& number,
                            std::optional<std::uint32_t>& total) {
    const std::size_t slash = text.find('/');
    const std::string numPart = slash == std::string::npos ? text : text.substr(0, slash);
    if (!numPart.empty() && std::all_of(numPart.begin(), numPart.end(), isAsciiDigit)) {
        number = static_cast<std::uint32_t>(std::strtoul(numPart.c_str(), nullptr, 10));
    }
    if (slash != std::string::npos) {
        const std::string totalPart = text.substr(slash + 1);
        if (!totalPart.empty() && std::all_of(totalPart.begin(), totalPart.end(), isAsciiDigit)) {
            total = static_cast<std::uint32_t>(std::strtoul(totalPart.c_str(), nullptr, 10));
        }
    }
}

std::optional<std::uint32_t> parseUintPrefix(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    if (i == 0) return std::nullopt;
    return static_cast<std::uint32_t>(std::strtoul(text.substr(0, i).c_str(), nullptr, 10));
}

void handleTxxx(const std::string& description, const std::string& value, const std::string& sourceFormat,
                TagSet& tags) {
    std::string upperDesc = description;
    std::transform(upperDesc.begin(), upperDesc.end(), upperDesc.begin(), ::toupper);

    if (upperDesc == "REPLAYGAIN_TRACK_GAIN" || upperDesc == "REPLAYGAIN_ALBUM_GAIN") {
        ReplayGainSource src;
        src.origin = "id3v2.txxx";
        if (upperDesc == "REPLAYGAIN_TRACK_GAIN") src.trackGainDb = parseReplayGainDb(value);
        else src.albumGainDb = parseReplayGainDb(value);
        tags.replayGainSources.push_back(src);
        return;
    }
    if (upperDesc == "REPLAYGAIN_TRACK_PEAK" || upperDesc == "REPLAYGAIN_ALBUM_PEAK") {
        ReplayGainSource src;
        src.origin = "id3v2.txxx";
        if (upperDesc == "REPLAYGAIN_TRACK_PEAK") src.trackPeak = parseReplayGainPeak(value);
        else src.albumPeak = parseReplayGainPeak(value);
        tags.replayGainSources.push_back(src);
        return;
    }
    if (upperDesc == "MUSICBRAINZ TRACK ID" || upperDesc == "MUSICBRAINZ_TRACKID") {
        if (!tags.musicBrainzTrackId) tags.musicBrainzTrackId = value;
        return;
    }
    if (upperDesc == "MUSICBRAINZ ALBUM ID" || upperDesc == "MUSICBRAINZ_ALBUMID") {
        if (!tags.musicBrainzAlbumId) tags.musicBrainzAlbumId = value;
        return;
    }
    if (upperDesc == "CATALOGNUMBER") {
        if (!tags.catalogNumber) tags.catalogNumber = value;
        return;
    }
    if (upperDesc == "BARCODE" || upperDesc == "UPC") {
        if (!tags.upc) tags.upc = value;
        return;
    }

    tags.unmapped.emplace_back("TXXX:" + description, MetadataValue{value, sourceFormat, "TXXX:" + description});
}

void handleRva2(std::span<const std::byte> body, TagSet& tags) {
    std::size_t offset = 0;
    (void)readTerminatedString(body, offset, TextEncoding::Latin1);  // identifier — not otherwise used

    std::optional<double> masterGain, firstGain;
    while (offset + 4 <= body.size()) {
        const std::uint8_t channelType = static_cast<std::uint8_t>(body[offset]);
        const auto         rawGain     = static_cast<std::int16_t>(
            (static_cast<std::uint16_t>(body[offset + 1]) << 8) | static_cast<std::uint16_t>(body[offset + 2]));
        const double gainDb = static_cast<double>(rawGain) / 512.0;
        offset += 3;
        if (offset >= body.size()) break;
        const std::uint8_t peakBits  = static_cast<std::uint8_t>(body[offset]);
        ++offset;
        const std::size_t peakBytes = (static_cast<std::size_t>(peakBits) + 7) / 8;
        if (offset + peakBytes > body.size()) break;
        offset += peakBytes;

        if (!firstGain) firstGain = gainDb;
        if (channelType == 1) masterGain = gainDb;  // 1 == "Master volume"
    }

    if (masterGain || firstGain) {
        ReplayGainSource src;
        src.origin      = "id3v2.rva2";
        src.trackGainDb = masterGain ? masterGain : firstGain;
        tags.replayGainSources.push_back(src);
    }
}

void handleChap(std::span<const std::byte> body, int majorVersion, const std::string& sourceFormat, TagSet& tags) {
    std::size_t offset = 0;
    std::string elementId = readTerminatedString(body, offset, TextEncoding::Latin1);
    if (offset + 16 > body.size()) return;  // truncated fixed-size fields — nothing safe to read

    const std::uint32_t startMs = plainBeToUint32(body.subspan(offset, 4));
    offset += 16;  // start ms, end ms, start offset, end offset — only start time is used below

    // Best-effort: scan any embedded sub-frames for a TIT2 to use as a human label. Sub-frames use
    // the same header shape as the parent version; a corrupt/short sub-frame region just means no
    // label is found, not a parse failure for the chapter itself.
    std::string label = elementId;
    const std::size_t headerLen = majorVersion >= 3 ? 10 : 6;
    std::size_t        cursor    = offset;
    while (cursor + headerLen <= body.size()) {
        std::string subId;
        std::uint32_t subSize;
        if (majorVersion >= 3) {
            subId = std::string(reinterpret_cast<const char*>(body.data() + cursor), 4);
            subSize = majorVersion == 4 ? syncsafeToUint32(body.subspan(cursor + 4, 4))
                                        : plainBeToUint32(body.subspan(cursor + 4, 4));
            cursor += 10;
        } else {
            subId   = std::string(reinterpret_cast<const char*>(body.data() + cursor), 3);
            subSize = plainBeToUint24(body.subspan(cursor + 3, 3));
            cursor += 6;
        }
        if (subId.empty() || subId[0] == '\0') break;
        if (cursor + subSize > body.size()) break;

        if (canonicalFrameId(majorVersion, subId) == "TIT2" && subSize >= 1) {
            std::size_t textOffset = 1;
            auto        subBody    = body.subspan(cursor, subSize);
            label = decodeToUtf8(subBody.subspan(textOffset), textEncodingFromByte(static_cast<std::uint8_t>(subBody[0])));
            break;
        }
        cursor += subSize;
    }

    CuePoint cp;
    cp.timeSeconds  = static_cast<double>(startMs) / 1000.0;
    cp.label        = label;
    cp.sourceFormat = sourceFormat;
    tags.cuePoints.push_back(std::move(cp));
}

void dispatchFrame(const std::string& canonicalId, const std::string& rawId, std::span<const std::byte> body,
                    int majorVersion, const std::string& sourceFormat, TagSet& tags) {
    if (body.empty() && canonicalId != "CTOC") {
        return;  // nothing to read; an empty frame is not an error
    }

    if (canonicalId == "TXXX") {
        std::size_t offset  = 1;
        const auto  enc     = textEncodingFromByte(static_cast<std::uint8_t>(body[0]));
        std::string desc    = readTerminatedString(body, offset, enc);
        std::string value   = decodeToUtf8(body.subspan(offset), enc);
        handleTxxx(desc, value, sourceFormat, tags);
        return;
    }

    if (canonicalId == "COMM") {
        if (body.size() < 4) return;
        const auto  enc     = textEncodingFromByte(static_cast<std::uint8_t>(body[0]));
        std::size_t offset  = 4;  // encoding byte + 3-byte language
        (void)readTerminatedString(body, offset, enc);  // short content descriptor — discarded
        std::string text = decodeToUtf8(body.subspan(offset), enc);
        if (!tags.comment) tags.comment = text;
        return;
    }

    if (canonicalId == "USLT") {
        if (body.size() < 4) return;
        const auto  enc     = textEncodingFromByte(static_cast<std::uint8_t>(body[0]));
        std::size_t offset  = 4;
        std::string language(reinterpret_cast<const char*>(body.data() + 1), 3);
        std::string descriptor = readTerminatedString(body, offset, enc);
        std::string text        = decodeToUtf8(body.subspan(offset), enc);
        tags.lyrics.push_back(makeLyricsFromUnsyncedText(std::move(text), language, descriptor, sourceFormat));
        return;
    }

    if (canonicalId == "SYLT") {
        if (body.size() < 6) return;
        const auto  enc              = textEncodingFromByte(static_cast<std::uint8_t>(body[0]));
        std::string language(reinterpret_cast<const char*>(body.data() + 1), 3);
        const std::uint8_t timestampFormat = static_cast<std::uint8_t>(body[4]);
        std::size_t         offset          = 6;
        std::string          descriptor      = readTerminatedString(body, offset, enc);

        if (timestampFormat != 2) {
            tags.diagnostics.push_back(
                {Severity::Info, sourceFormat + ": SYLT uses MPEG-frame timestamps, which aren't supported; skipped"});
            return;
        }

        Lyrics lyr;
        lyr.synced      = true;
        lyr.description = descriptor;
        lyr.sourceFormat = sourceFormat;
        while (offset < body.size()) {
            std::string lineText = readTerminatedString(body, offset, enc);
            if (offset + 4 > body.size()) break;
            const std::uint32_t ms = plainBeToUint32(body.subspan(offset, 4));
            offset += 4;
            lyr.lines.push_back(LyricLine{static_cast<double>(ms) / 1000.0, lineText});
        }
        tags.lyrics.push_back(std::move(lyr));
        return;
    }

    if (canonicalId == "APIC" || (majorVersion == 2 && rawId == "PIC")) {
        const auto  enc = textEncodingFromByte(static_cast<std::uint8_t>(body[0]));
        std::size_t offset = 1;
        std::string mimeType;
        if (majorVersion == 2) {
            if (body.size() < 1 + 3 + 1) return;
            mimeType = std::string(reinterpret_cast<const char*>(body.data() + 1), 3);
            offset   = 4;
        } else {
            mimeType = readTerminatedString(body, offset, TextEncoding::Latin1);
        }
        if (offset >= body.size()) return;
        const auto pictureType = static_cast<PictureType>(static_cast<std::uint8_t>(body[offset]));
        ++offset;
        std::string description = readTerminatedString(body, offset, enc);

        Picture pic;
        if (extractPicture(body.subspan(offset), mimeType, pictureType, description, sourceFormat, pic,
                            tags.diagnostics)) {
            tags.pictures.push_back(std::move(pic));
        }
        return;
    }

    if (canonicalId == "RVA2") {
        handleRva2(body, tags);
        return;
    }

    if (canonicalId == "CHAP") {
        handleChap(body, majorVersion, sourceFormat, tags);
        return;
    }

    if (canonicalId == "CTOC") {
        tags.unmapped.emplace_back("CTOC", MetadataValue{"<table of contents, not expanded>", sourceFormat, "CTOC"});
        return;
    }

    if (canonicalId == "PRIV") {
        std::size_t offset = 0;
        std::string owner  = readTerminatedString(body, offset, TextEncoding::Latin1);
        tags.unmapped.emplace_back("PRIV:" + owner,
                                    MetadataValue{"<" + std::to_string(body.size() - offset) + " binary bytes>",
                                                  sourceFormat, "PRIV:" + owner});
        return;
    }

    // Generic text/URL frames (T*** except TXXX already handled, W*** except WXXX).
    if (!rawId.empty() && (rawId[0] == 'T' || rawId[0] == 'W')) {
        std::string text;
        if (rawId[0] == 'T') {
            const auto enc = textEncodingFromByte(static_cast<std::uint8_t>(body[0]));
            text            = decodeToUtf8(body.subspan(1), enc);
            auto values     = splitNulSeparated(text);
            if (!values.empty()) text = values.front();
            for (std::size_t i = 1; i < values.size(); ++i) text += "; " + values[i];
        } else {
            text = decodeToUtf8(body, TextEncoding::Latin1);
        }

        if (canonicalId == "TIT2" && !tags.title) tags.title = text;
        else if (canonicalId == "TPE1" && !tags.artist) tags.artist = text;
        else if (canonicalId == "TPE2" && !tags.albumArtist) tags.albumArtist = text;
        else if (canonicalId == "TALB" && !tags.album) tags.album = text;
        else if (canonicalId == "TCON" && !tags.genre) tags.genre = text;
        else if (canonicalId == "TCOM" && !tags.composer) tags.composer = text;
        else if (canonicalId == "TPUB" && !tags.publisher) tags.publisher = text;
        else if (canonicalId == "TCOP" && !tags.copyright) tags.copyright = text;
        else if (canonicalId == "TENC" && !tags.encodedBy) tags.encodedBy = text;
        else if (canonicalId == "TSSE" && !tags.encoderSettings) tags.encoderSettings = text;
        else if (canonicalId == "TSRC" && !tags.isrc) tags.isrc = text;
        else if (canonicalId == "TBPM" && !tags.bpm) tags.bpm = parseUintPrefix(text);
        else if (canonicalId == "TPOS") parseNumberSlashTotal(text, tags.discNumber, tags.discTotal);
        else if (canonicalId == "TRCK") parseNumberSlashTotal(text, tags.trackNumber, tags.trackTotal);
        else if (canonicalId == "TYER" && !tags.year) tags.year = parseUintPrefix(text);
        else if (canonicalId == "TDRC") {
            if (!tags.date) tags.date = text;
            if (!tags.year) tags.year = parseUintPrefix(text);
        } else {
            tags.unmapped.emplace_back(rawId, MetadataValue{text, sourceFormat, rawId});
        }
        return;
    }

    tags.unmapped.emplace_back(
        rawId, MetadataValue{"<" + std::to_string(body.size()) + " binary bytes>", sourceFormat, rawId});
}

}  // namespace

Id3v2ParseResult parseId3v2(std::span<const std::byte> bytes) {
    Id3v2ParseResult result;

    if (bytes.size() < 10 || static_cast<char>(bytes[0]) != 'I' || static_cast<char>(bytes[1]) != 'D' ||
        static_cast<char>(bytes[2]) != '3') {
        return result;  // present = false
    }

    const auto majorVersion = static_cast<std::uint8_t>(bytes[3]);
    const auto flags        = static_cast<std::uint8_t>(bytes[5]);
    const std::uint32_t tagSize = syncsafeToUint32(bytes.subspan(6, 4));

    if (majorVersion < 2 || majorVersion > 4) {
        result.tags.diagnostics.push_back(
            {Severity::Warning, "id3v2: unrecognised major version " + std::to_string(majorVersion) + ", skipped"});
        return result;
    }

    result.present     = true;
    result.totalTagSize = 10 + tagSize;
    const std::string sourceFormat = "id3v2." + std::to_string(majorVersion);
    result.tags.sourceFormat        = sourceFormat;

    const bool unsyncFlag    = (flags & 0x80) != 0;
    const bool extHeaderFlag = (flags & 0x40) != 0 && majorVersion >= 3;

    if (10u + tagSize > bytes.size()) {
        result.tags.diagnostics.push_back(
            {Severity::Warning, sourceFormat + ": declared tag size extends past the available data; "
                                                "parsing what's present"});
    }
    const std::size_t available = std::min<std::size_t>(10u + tagSize, bytes.size()) - 10u;
    std::span<const std::byte> payload = bytes.subspan(10, available);

    std::size_t extHeaderBytes = 0;
    if (extHeaderFlag && payload.size() >= 4) {
        if (majorVersion == 4) {
            const std::uint32_t size = syncsafeToUint32(payload.subspan(0, 4));
            extHeaderBytes            = size <= payload.size() ? size : payload.size();
        } else {
            const std::uint32_t size = plainBeToUint32(payload.subspan(0, 4));
            extHeaderBytes            = (4u + size) <= payload.size() ? 4u + size : payload.size();
        }
        payload = payload.subspan(extHeaderBytes);
    }

    // Tag-level unsynchronisation applies uniformly to everything from here (all of v2.2/v2.3's
    // scheme, and v2.4 files that set the tag-level flag instead of per-frame ones) — decode once,
    // up front, and parse frames against the desynced copy.
    std::vector<std::byte> desyncedStorage;
    if (unsyncFlag) {
        desyncedStorage = removeUnsynchronisation(payload);
        payload          = std::span<const std::byte>(desyncedStorage);
    }

    const std::size_t headerLen = majorVersion >= 3 ? 10 : 6;
    const std::size_t idLen     = majorVersion >= 3 ? 4 : 3;
    std::size_t        cursor    = 0;

    while (cursor + headerLen <= payload.size()) {
        const auto idBytes = payload.subspan(cursor, idLen);
        if (static_cast<char>(idBytes[0]) == '\0') break;  // padding reached

        bool idIsPrintable = true;
        for (std::byte b : idBytes) {
            const auto c = static_cast<std::uint8_t>(b);
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                idIsPrintable = false;
                break;
            }
        }
        if (!idIsPrintable) break;  // not a real frame id — treat the rest as padding/garbage, stop

        const std::string rawId(reinterpret_cast<const char*>(idBytes.data()), idLen);

        std::uint32_t frameSize;
        std::uint16_t frameFlags = 0;
        if (majorVersion == 2) {
            frameSize = plainBeToUint24(payload.subspan(cursor + 3, 3));
        } else {
            const auto sizeBytes = payload.subspan(cursor + 4, 4);
            frameSize             = majorVersion == 4 ? syncsafeToUint32(sizeBytes) : plainBeToUint32(sizeBytes);
            frameFlags            = (static_cast<std::uint16_t>(payload[cursor + 8]) << 8) |
                          static_cast<std::uint16_t>(payload[cursor + 9]);
        }

        const std::size_t frameStart = cursor + headerLen;
        if (frameStart + frameSize > payload.size()) {
            result.tags.diagnostics.push_back(
                {Severity::Warning, sourceFormat + ": frame '" + rawId + "' declares a size that overruns the tag; "
                                                    "stopping frame scan"});
            break;
        }

        std::span<const std::byte> frameBody = payload.subspan(frameStart, frameSize);

        // v2.4 per-frame flags: format byte is the low byte of frameFlags.
        const std::uint8_t formatFlags = static_cast<std::uint8_t>(frameFlags & 0xFF);
        const bool compressed          = majorVersion == 4 ? (formatFlags & 0x08) != 0 : (formatFlags & 0x80) != 0;
        const bool encrypted           = majorVersion == 4 ? (formatFlags & 0x04) != 0 : (formatFlags & 0x40) != 0;
        const bool frameUnsync         = majorVersion == 4 && (formatFlags & 0x02) != 0;
        const bool hasDataLenIndicator = majorVersion == 4 && (formatFlags & 0x01) != 0;

        std::vector<std::byte> frameStorage;
        if (frameUnsync && !unsyncFlag) {  // avoid decoding twice if the whole tag was already desynced
            frameStorage = removeUnsynchronisation(frameBody);
            frameBody     = std::span<const std::byte>(frameStorage);
        }
        if (hasDataLenIndicator && frameBody.size() >= 4) {
            frameBody = frameBody.subspan(4);  // skip the syncsafe "expanded size" field — not otherwise used
        }

        if (compressed || encrypted) {
            result.tags.diagnostics.push_back(
                {Severity::Info, sourceFormat + ": frame '" + rawId + "' is " +
                                      (compressed ? std::string("compressed") : std::string("encrypted")) +
                                      "; not supported, skipped"});
        } else {
            dispatchFrame(canonicalFrameId(majorVersion, rawId), rawId, frameBody, majorVersion, sourceFormat,
                          result.tags);
        }

        cursor = frameStart + frameSize;
    }

    return result;
}

}  // namespace aud::metadata

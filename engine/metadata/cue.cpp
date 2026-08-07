#include "cue.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>

namespace aud::metadata {

namespace {

std::string trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

// Extracts the quoted argument of a `KEYWORD "value"` line, or empty if there isn't one.
std::string quotedArg(const std::string& line) {
    const std::size_t open = line.find('"');
    if (open == std::string::npos) return {};
    const std::size_t close = line.find('"', open + 1);
    if (close == std::string::npos) return {};
    return line.substr(open + 1, close - open - 1);
}

// Parses "mm:ss:ff" (CD frames, 75/sec) into seconds.
std::optional<double> parseCueTimestamp(const std::string& text) {
    int mm = 0, ss = 0, ff = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d", &mm, &ss, &ff) != 3) return std::nullopt;
    if (ss < 0 || ss > 59 || ff < 0 || ff > 74) return std::nullopt;
    return static_cast<double>(mm) * 60.0 + static_cast<double>(ss) + static_cast<double>(ff) / 75.0;
}

}  // namespace

std::vector<CuePoint> parseExternalCueSheet(const std::string& text) {
    std::vector<CuePoint> points;

    bool                   inTrack        = false;
    std::string            trackTitle;
    std::optional<double>  index00, index01;
    int                     trackNumber    = 0;

    auto flushTrack = [&]() {
        if (!inTrack) return;
        const std::optional<double>& chosen = index01 ? index01 : index00;
        if (chosen) {
            CuePoint cp;
            cp.timeSeconds = *chosen;
            cp.label       = !trackTitle.empty() ? trackTitle : ("Track " + std::to_string(trackNumber));
            cp.sourceFormat = "cue";
            points.push_back(std::move(cp));
        }
        inTrack = false;
        trackTitle.clear();
        index00.reset();
        index01.reset();
    };

    std::istringstream stream(text);
    std::string        rawLine;
    while (std::getline(stream, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == '\r') rawLine.pop_back();
        const std::string line = trim(rawLine);
        if (line.empty()) continue;

        if (line.compare(0, 5, "TRACK") == 0) {
            flushTrack();
            inTrack = true;
            std::sscanf(line.c_str(), "TRACK %d", &trackNumber);
        } else if (inTrack && line.compare(0, 5, "TITLE") == 0) {
            trackTitle = quotedArg(line);
        } else if (inTrack && line.compare(0, 5, "INDEX") == 0) {
            int         indexNum = -1;
            char        tsBuf[32] = {};
            if (std::sscanf(line.c_str(), "INDEX %d %31s", &indexNum, tsBuf) == 2) {
                if (auto seconds = parseCueTimestamp(tsBuf)) {
                    if (indexNum == 1) {
                        index01 = seconds;
                    } else if (indexNum == 0) {
                        index00 = seconds;
                    }
                }
            }
        }
        // FILE/PERFORMER/CATALOG/REM and anything else are ignored here — best-effort per M15's
        // scope note; the album-level PERFORMER/TITLE aren't cue points and belong to a future
        // "read the whole cue sheet as metadata" feature, not this one.
    }
    flushTrack();

    return points;
}

std::vector<CuePoint> parseFlacCueSheet(std::span<const std::byte> body, std::uint32_t sampleRate) {
    std::vector<CuePoint> points;
    if (sampleRate == 0) return points;

    // Layout (FLAC spec): 128-byte catalog number, 8-byte lead-in sample count, 1 byte
    // (bit7=is-CD flag + 7 reserved bits), 258 reserved bytes, 1 byte track count, then that many
    // track entries: 8-byte offset, 1-byte track number, 12-byte ISRC, 1 byte (bit7=audio flag +
    // reserved), 13 reserved bytes, 1-byte index-point count, then that many index entries:
    // 8-byte offset, 1-byte index number, 3 reserved bytes.
    constexpr std::size_t kHeaderSize = 128 + 8 + 1 + 258 + 1;
    if (body.size() < kHeaderSize) return points;  // truncated — nothing to salvage safely

    auto readU64Be = [](std::span<const std::byte> b, std::size_t offset) -> std::uint64_t {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint8_t>(b[offset + i]);
        return v;
    };

    const std::uint8_t trackCount = static_cast<std::uint8_t>(body[128 + 8 + 1 + 258]);
    std::size_t         cursor     = kHeaderSize;

    for (std::uint8_t t = 0; t < trackCount; ++t) {
        constexpr std::size_t kTrackFixedSize = 8 + 1 + 12 + 1 + 13 + 1;
        if (cursor + kTrackFixedSize > body.size()) break;  // truncated track table — stop, don't read past it

        const std::uint64_t trackOffset = readU64Be(body, cursor);
        const std::uint8_t  trackNumber = static_cast<std::uint8_t>(body[cursor + 8]);
        const std::uint8_t  indexCount  = static_cast<std::uint8_t>(body[cursor + 8 + 1 + 12 + 1 + 13]);
        cursor += kTrackFixedSize;

        // Track 170 (0xAA) is FLAC's lead-out marker — carries no index points and isn't a playable track.
        const bool isLeadOut = trackNumber == 0xAA;

        std::optional<std::uint64_t> firstIndexOffset;
        for (std::uint8_t i = 0; i < indexCount; ++i) {
            constexpr std::size_t kIndexSize = 8 + 1 + 3;
            if (cursor + kIndexSize > body.size()) {
                cursor = body.size();  // stop everything — table is corrupt past this point
                break;
            }
            const std::uint64_t indexOffset = readU64Be(body, cursor);
            const std::uint8_t  indexNumber = static_cast<std::uint8_t>(body[cursor + 8]);
            cursor += kIndexSize;

            if (indexNumber == 1 || (!firstIndexOffset.has_value() && indexNumber == 0)) {
                firstIndexOffset = trackOffset + indexOffset;
            }
        }

        if (!isLeadOut && firstIndexOffset.has_value()) {
            CuePoint cp;
            cp.timeSeconds  = static_cast<double>(*firstIndexOffset) / static_cast<double>(sampleRate);
            cp.label        = "Track " + std::to_string(trackNumber);
            cp.sourceFormat = "flac.cuesheet";
            points.push_back(std::move(cp));
        }

        if (cursor >= body.size()) break;
    }

    return points;
}

}  // namespace aud::metadata

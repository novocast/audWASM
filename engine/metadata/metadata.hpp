#pragma once

// M15's unified metadata model — see documentation/tasks/M15-metadata.md.
//
// Four incompatible tagging systems (ID3v2/ID3v1/APEv2 for MP3, Vorbis comments for FLAC/OGG,
// iTunes `ilst` atoms for MP4, RIFF LIST/INFO + BWF `bext` + iXML for WAV) get normalised into one
// struct here. Two decisions drive its shape (both from the milestone doc):
//
//   - "Always preserve unmapped tags" — anything we don't recognise still round-trips into
//     `unmapped`, tagged with its original key and source format, rather than being silently
//     dropped. A QA tool whose entire value proposition is "tell me what's in this file" cannot
//     drop the one custom tag the user was actually looking for.
//   - "Preserve which format a field came from, and report conflicts rather than resolving them
//     silently" — when ID3v1 and ID3v2 disagree on the artist (common), the caller needs to see
//     both, not just the winner. `fieldConflicts` carries the losing (and, for symmetry, winning)
//     values whenever two sources disagree on a mapped field.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../util/result.hpp"

namespace aud::metadata {

enum class Severity : std::uint8_t { Info, Warning, Error };

struct Diagnostic {
    Severity    severity = Severity::Info;
    std::string message;
};

// One raw tag value, tagged with which format produced it and its original key — never silently
// overwritten across formats.
struct MetadataValue {
    std::string text;
    std::string sourceFormat;  // "id3v2.4", "id3v2.3", "id3v2.2", "id3v1", "apev2", "vorbis", "mp4", "riff"
    std::string rawKey;        // the original tag key/frame id, preserved verbatim
};

// FLAC PICTURE / ID3 APIC "picture type" byte — identical enumeration in both specs (ID3 borrowed
// FLAC's numbering... actually the reverse; either way the two specs agree byte-for-byte).
enum class PictureType : std::uint8_t {
    Other              = 0,
    FileIcon           = 1,
    OtherFileIcon      = 2,
    FrontCover         = 3,
    BackCover          = 4,
    LeafletPage        = 5,
    Media              = 6,
    LeadArtist         = 7,
    Artist             = 8,
    Conductor          = 9,
    Band               = 10,
    Composer           = 11,
    Lyricist           = 12,
    RecordingLocation  = 13,
    DuringRecording    = 14,
    DuringPerformance  = 15,
    VideoScreenCapture = 16,
    Fish               = 17,
    Illustration       = 18,
    BandLogo           = 19,
    PublisherLogo      = 20,
};

struct Picture {
    std::vector<std::byte> data;
    std::string             declaredMimeType;  // as the tag itself claims
    std::string             detectedMimeType;  // sniffed from magic bytes; empty if unrecognised
    bool                     mimeMismatch = false;
    PictureType              type = PictureType::Other;
    std::string              description;
    std::string              sourceFormat;
};

struct LyricLine {
    double      timeSeconds = -1.0;  // negative => unsynced / position unknown
    std::string text;
};

struct Lyrics {
    bool                    synced = false;
    std::string             language;  // ISO-639-2, if known; empty otherwise
    std::string             description;
    std::vector<LyricLine>  lines;      // unsynced: exactly one line, timeSeconds < 0
    std::string             sourceFormat;
};

struct CuePoint {
    double      timeSeconds = 0.0;
    std::string label;
    std::string sourceFormat;
};

// One ReplayGain reading. `sources`, in `ReplayGainInfo`, are kept in the milestone doc's stated
// priority order (Vorbis/TXXX > RVA2 > iTunNORM) rather than merged — different sources can and do
// disagree, and that disagreement is itself diagnostic information.
struct ReplayGainSource {
    std::optional<double> trackGainDb;
    std::optional<double> trackPeak;
    std::optional<double> albumGainDb;
    std::optional<double> albumPeak;
    std::string            origin;  // "vorbis", "id3v2.rva2", "mp4.itunnorm"
};

struct ReplayGainInfo {
    std::vector<ReplayGainSource> sources;

    [[nodiscard]] const ReplayGainSource* preferred() const noexcept {
        return sources.empty() ? nullptr : &sources.front();
    }
};

// WAV `bext` (Broadcast Wave Format) chunk — fixed layout, BWF versions 0-2 (v2 adds the loudness
// fields; parser fills what it can and leaves the rest default).
struct BroadcastInfo {
    bool          present = false;
    std::string   description;
    std::string   originator;
    std::string   originatorReference;
    std::string   originationDate;  // "YYYY-MM-DD" as stored
    std::string   originationTime;  // "HH:MM:SS" as stored
    std::uint64_t timeReference = 0;  // sample-accurate start timecode, in samples at the file's rate
    std::uint16_t version       = 0;
    std::string   umid;             // 64-byte UMID, hex-encoded
    std::string   codingHistory;

    // BWF v2 loudness fields (LUFS/LU, stored as the spec's int16 hundredths); absent (nullopt) on
    // v0/v1.
    std::optional<double> loudnessValueLufs;
    std::optional<double> loudnessRangeLu;
    std::optional<double> maxTruePeakDbtp;
    std::optional<double> maxMomentaryLufs;
    std::optional<double> maxShortTermLufs;
};

struct Metadata {
    std::optional<std::string> title, artist, albumArtist, album, genre, composer, comment, publisher,
        copyright, encodedBy, encoderSettings;
    std::optional<std::uint32_t> year, trackNumber, trackTotal, discNumber, discTotal, bpm;
    std::optional<std::string>   isrc, upc, catalogNumber, musicBrainzTrackId, musicBrainzAlbumId;
    std::optional<std::string>   date;  // ISO-8601 where parseable, raw text otherwise

    ReplayGainInfo         replayGain;
    std::vector<Picture>   pictures;
    std::vector<Lyrics>    lyrics;
    std::vector<CuePoint>  cuePoints;
    BroadcastInfo          broadcast;

    // Everything we didn't map into a named field above, verbatim.
    std::vector<std::pair<std::string, MetadataValue>> unmapped;

    // Populated only when two-or-more sources disagree on a mapped field; key is the field name
    // ("artist", "title", ...). Every disagreeing value is listed (including the one that won).
    std::vector<std::pair<std::string, MetadataValue>> fieldConflicts;

    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] std::string toJson() const;
};

// Extracts metadata from a complete, in-memory copy of the file. Unlike the decoder's probe-based
// sniff() (which only needs the first ~64KB to identify a codec), tag data can require seeing
// arbitrarily far into the file — a trailing ID3v1 tag, an oversized APIC block, MP4 atoms located
// after `mdat` — so this always wants the whole file's bytes.
Result<Metadata> extract(std::span<const std::byte> fileBytes);

}  // namespace aud::metadata

#pragma once

// Intermediate representation each format-specific parser (id3v2, id3v1, apev2, vorbis_comment,
// mp4::ilst, riff) produces. `metadata_extractor.cpp` merges a priority-ordered list of these into
// the final aud::metadata::Metadata — see that file for the merge/conflict rules.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "metadata.hpp"

namespace aud::metadata {

struct TagSet {
    std::string sourceFormat;  // "id3v2.4", "id3v2.3", "id3v2.2", "id3v1", "apev2", "vorbis", "mp4", "riff"

    std::optional<std::string> title, artist, albumArtist, album, genre, composer, comment, publisher,
        copyright, encodedBy, encoderSettings;
    std::optional<std::uint32_t> year, trackNumber, trackTotal, discNumber, discTotal, bpm;
    std::optional<std::string>   isrc, upc, catalogNumber, musicBrainzTrackId, musicBrainzAlbumId;
    std::optional<std::string>   date;

    std::vector<ReplayGainSource> replayGainSources;
    std::vector<Picture>          pictures;
    std::vector<Lyrics>           lyrics;
    std::vector<CuePoint>         cuePoints;
    BroadcastInfo                 broadcast;

    std::vector<std::pair<std::string, MetadataValue>> unmapped;  // key = raw tag key
    std::vector<Diagnostic>                             diagnostics;
};

// Merges `sets` (already ordered highest-priority first, e.g. ID3v2.4 > ID3v2.3 > APEv2 > ID3v1)
// into a single Metadata. Never resolves a disagreement silently: when two sets both supply a
// mapped field with different text, every value is appended to `fieldConflicts` (the winner too,
// for symmetry) alongside the winning field itself.
[[nodiscard]] Metadata mergeTagSets(const std::vector<TagSet>& sets);

// Merges `src` into `dst` in place: each scalar field is taken from `src` only if `dst` doesn't
// already have one (no conflict tracking — this is for combining multiple fragments of the *same*
// source format, e.g. more than one VORBIS_COMMENT block in one FLAC file, not for cross-format
// conflict reporting), and every collection field is concatenated.
void appendTagSet(TagSet& dst, TagSet src);

}  // namespace aud::metadata

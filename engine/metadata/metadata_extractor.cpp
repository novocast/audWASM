// Top-level dispatch: sniffs the container and gathers every tagging system it can carry, then
// merges them via tag_set.hpp's priority-ordered mergeTagSets() — see M15 "Four tagging systems,
// one model" and "Conflict resolution".
//
// Unlike the decoder's format_sniffer.cpp (which only needs the first ~64KB to identify a codec),
// this always operates on the complete file: a trailing ID3v1 tag, an oversized APIC block, or MP4
// atoms placed after `mdat` all require seeing arbitrarily far into the file.

#include <cstring>

#include "apev2.hpp"
#include "id3v1.hpp"
#include "id3v2.hpp"
#include "metadata.hpp"
#include "mp4/ilst.hpp"
#include "riff.hpp"
#include "tag_set.hpp"
#include "vorbis_comment.hpp"

namespace aud::metadata {

namespace {

bool matches(std::span<const std::byte> bytes, std::size_t offset, const char* literal, std::size_t len) {
    if (bytes.size() < offset + len) return false;
    return std::memcmp(bytes.data() + offset, literal, len) == 0;
}

}  // namespace

Result<Metadata> extract(std::span<const std::byte> fileBytes) {
    std::vector<TagSet> sets;  // priority order: highest-priority source first

    if (matches(fileBytes, 0, "fLaC", 4)) {
        FlacParseResult flac = parseFlacMetadataBlocks(fileBytes);
        if (flac.present) sets.push_back(std::move(flac.tags));
    } else if (matches(fileBytes, 0, "OggS", 4)) {
        OggParseResult ogg = parseOggVorbisComment(fileBytes);
        if (ogg.present) sets.push_back(std::move(ogg.tags));
    } else if (matches(fileBytes, 0, "RIFF", 4) && matches(fileBytes, 8, "WAVE", 4)) {
        RiffParseResult riff = parseRiff(fileBytes);
        if (riff.present) sets.push_back(std::move(riff.tags));
    } else if (matches(fileBytes, 4, "ftyp", 4)) {
        sets.push_back(mp4::parseIlst(fileBytes));
    } else {
        // MP3 (or unrecognised): all three of ID3v2 (front), APEv2, and ID3v1 (both trailing) can
        // coexist in one file — try all three, highest-priority first (M15's stated order:
        // ID3v2.4 > ID3v2.3 > APEv2 > ID3v1; a single file only ever has one ID3v2 tag, whichever
        // version it is, so that part of the ordering is automatic).
        Id3v2ParseResult id3v2 = parseId3v2(fileBytes);
        if (id3v2.present) sets.push_back(std::move(id3v2.tags));

        Apev2ParseResult ape = parseApev2(fileBytes);
        if (ape.present) sets.push_back(std::move(ape.tags));

        Id3v1ParseResult id3v1 = parseId3v1(fileBytes);
        if (id3v1.present) sets.push_back(std::move(id3v1.tags));
    }

    // No tags found in any recognised system: mergeTagSets({}) yields a default-constructed,
    // empty-but-valid Metadata — exactly M15's "no tags at all" acceptance criterion, not an error.
    return mergeTagSets(sets);
}

}  // namespace aud::metadata

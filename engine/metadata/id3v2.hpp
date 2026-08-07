#pragma once

// ID3v2.2/2.3/2.4 tag parser — see documentation/tasks/M15-metadata.md "ID3 specifics that bite".
// Handles unsynchronisation (tag- and frame-level), the syncsafe-vs-plain frame size split between
// v2.3 and v2.4 (the classic bug the milestone doc calls out by name), extended headers/footers/
// padding, all four text encodings, and multi-value (0x00-separated) v2.4 text frames.
//
// Reusable beyond "ID3v2 tag glued to the front of an MP3": WAV's `id3 ` chunk (M15's RIFF row)
// embeds a complete ID3v2 tag, byte-for-byte the same format, so this parser takes a span starting
// at the "ID3" magic rather than assuming it owns the whole file.

#include <cstddef>
#include <span>

#include "tag_set.hpp"

namespace aud::metadata {

struct Id3v2ParseResult {
    bool        present      = false;
    TagSet      tags;
    std::size_t totalTagSize = 0;  // header + frames/padding, NOT including a footer (matches the header's own size field)
};

// `bytes` must start at the tag's "ID3" magic (offset 0 of the span). Returns present=false (not
// an error) if the magic doesn't match — callers sniff first. Never allocates more than the input
// size plus a small constant, regardless of what length fields inside the tag claim (M15's paranoia
// mandate: every length is validated against the remaining buffer before use).
Id3v2ParseResult parseId3v2(std::span<const std::byte> bytes);

}  // namespace aud::metadata

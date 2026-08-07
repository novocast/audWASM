#pragma once

// ID3v1 / ID3v1.1 — the 128-byte trailing tag. Always the lowest-priority MP3 source (M15's
// conflict order: ID3v2.4 > ID3v2.3 > APEv2 > ID3v1) but still worth reading in full: it's common
// for old rips to carry only this, and reporting "ID3v1 says X, ID3v2 says Y" needs both sides.

#include <span>

#include "tag_set.hpp"

namespace aud::metadata {

struct Id3v1ParseResult {
    bool   present = false;
    TagSet tags;
};

// `bytes` should be the whole file (or at least its final 128 bytes); the tag, if present, is the
// last 128 bytes and begins with "TAG". Handles the ID3v1.1 convention (byte 125 is 0 and byte 126
// is the track number) transparently.
Id3v1ParseResult parseId3v1(std::span<const std::byte> bytes);

}  // namespace aud::metadata

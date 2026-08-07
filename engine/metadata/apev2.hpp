#pragma once

// APEv2 — the "rare" MP3 tagging system named in M15's format table, and the tag sitting between
// ID3v2.3/2.4 and ID3v1 in the milestone doc's stated conflict-resolution order. Plain UTF-8
// key=value items, little-endian integers throughout (unlike ID3's big-endian/syncsafe schemes) —
// simple enough that supporting it costs little once ID3v1/v2 exist.

#include <span>

#include "tag_set.hpp"

namespace aud::metadata {

struct Apev2ParseResult {
    bool   present = false;
    TagSet tags;
};

// `bytes` should be the whole file. APEv2's footer (its most reliable anchor) sits at the very end
// of the file, or immediately before a trailing ID3v1 tag if one is also present — both locations
// are tried.
Apev2ParseResult parseApev2(std::span<const std::byte> bytes);

}  // namespace aud::metadata

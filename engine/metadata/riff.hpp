#pragma once

// WAV's RIFF-based metadata — see M15 "WAV" row: LIST/INFO chunk, BWF `bext`, an embedded `id3 `
// chunk (a byte-for-byte ID3v2 tag, reusing id3v2.cpp), best-effort iXML, plus the native RIFF
// `cue ` chunk (+ LIST/adtl `labl` labels) as a bonus cue-point source alongside M15's named ones.

#include <span>

#include "tag_set.hpp"

namespace aud::metadata {

struct RiffParseResult {
    bool   present = false;
    TagSet tags;
};

// `bytes` must start at "RIFF" (offset 0).
[[nodiscard]] RiffParseResult parseRiff(std::span<const std::byte> bytes);

}  // namespace aud::metadata

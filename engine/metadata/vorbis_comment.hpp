#pragma once

// Vorbis comments (clean, UTF-8, key=value) plus the FLAC metadata-block walker that carries them
// — see M15's "FLAC / OGG" row. One comment payload parser (`parseVorbisCommentPayload`) is shared
// by both containers: FLAC's native METADATA_BLOCK_HEADER framing and Ogg Vorbis's comment-header
// packet wrap the identical vendor-string + key=value list differently, but the payload itself is
// byte-for-byte the same structure either way.
//
// Also handles the FLAC METADATA_BLOCK_PICTURE Vorbis comment (base64-encoded FLAC PICTURE block —
// how FLAC-native cover art also gets carried inside plain Ogg Vorbis streams).

#include <cstdint>
#include <span>

#include "tag_set.hpp"

namespace aud::metadata {

// Parses the shared vendor-string + comment-list structure, starting at `payload[0]` being the
// 4-byte vendor length (i.e. with any container-specific header — FLAC's block header, Ogg's
// `\x03vorbis` packet-type prefix — already stripped by the caller).
[[nodiscard]] TagSet parseVorbisCommentPayload(std::span<const std::byte> payload, const std::string& sourceFormat);

struct FlacParseResult {
    bool          present    = false;
    TagSet        tags;
    std::uint32_t sampleRate = 0;  // from STREAMINFO; needed to convert CUESHEET sample offsets to seconds
};

// `bytes` should start at "fLaC" (offset 0). Walks every metadata block: STREAMINFO (for sample
// rate), VORBIS_COMMENT, PICTURE, and CUESHEET.
[[nodiscard]] FlacParseResult parseFlacMetadataBlocks(std::span<const std::byte> bytes);

struct OggParseResult {
    bool   present = false;
    TagSet tags;
};

// `bytes` should start at "OggS" (offset 0). Demuxes just enough of the Ogg page stream (the
// identification header packet plus the comment header packet) to reach the Vorbis comment header
// — full audio-packet demuxing is the decoder's job, not this one's.
[[nodiscard]] OggParseResult parseOggVorbisComment(std::span<const std::byte> bytes);

}  // namespace aud::metadata

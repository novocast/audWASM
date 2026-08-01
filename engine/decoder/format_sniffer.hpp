#pragma once

// Content sniffing, never trusting the file extension. See M02's detection ladder. Needs at most
// the first 64KB of a file, so it can run before the whole file has arrived.

#include <cstddef>
#include <span>

#include "../util/result.hpp"

namespace aud::decoder {

enum class ContainerFormat {
    Unknown,
    Wav,
    Aiff,
    Flac,
    OggVorbis,
    Mp3,
    Mp4Aac,   // ftyp brand indicates audio-only M4A/AAC
    Mp4Other, // ftyp brand indicates a container we should reject with a clear message (e.g. video)
};

struct SniffResult {
    ContainerFormat format = ContainerFormat::Unknown;
    std::size_t     headerBytesConsumed = 0;  // e.g. an ID3v2 tag skipped before the real sniff
};

// Sniffs `bytes` (ideally >= 64KB, but works with less — it just may be unable to distinguish
// some formats, e.g. it needs 3 consistent MP3 frame headers). Returns UnsupportedFormat with the
// first 16 bytes hex-dumped in Error::detail if nothing matches.
Result<SniffResult> sniff(std::span<const std::byte> bytes);

}  // namespace aud::decoder

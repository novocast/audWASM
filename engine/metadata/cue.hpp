#pragma once

// Cue point / cue sheet parsing — see M15 "Cue points / chapters". Two independent formats live
// here: FLAC's native binary CUESHEET metadata block, and external `.cue` sidecar files (plain
// text, the format DJ mixes and vinyl rips almost always ship alongside). ID3 CHAP/CTOC parsing
// lives in id3v2.cpp instead — its frame is embedded inside the ID3v2 frame stream and shares that
// file's frame-reading helpers, so splitting it out here would just duplicate them.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "metadata.hpp"

namespace aud::metadata {

// Parses an external `.cue` sheet's text (already read as a plain string; caller decides the
// source encoding — cue sheets are conventionally ASCII/Latin-1/UTF-8). One CuePoint per TRACK's
// INDEX 01 (falling back to INDEX 00 if 01 is absent), labelled from that track's own TITLE if
// present.
[[nodiscard]] std::vector<CuePoint> parseExternalCueSheet(const std::string& text);

// Parses a FLAC CUESHEET metadata block's raw body (the block-body bytes only, header already
// stripped by the caller). `sampleRate` converts the block's sample-offset fields to seconds.
[[nodiscard]] std::vector<CuePoint> parseFlacCueSheet(std::span<const std::byte> body, std::uint32_t sampleRate);

}  // namespace aud::metadata

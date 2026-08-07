#pragma once

// Lyrics helpers — see M15 "Lyrics". Unsynced lyric fields (ID3 USLT, Vorbis LYRICS, MP4 ©lyr) are
// plain text, but very commonly *contain* LRC-format synced lyrics anyway (`[mm:ss.xx]` per line) —
// detecting that turns a plain-text field into the same synced timeline overlay SYLT gets.

#include <string>

#include "metadata.hpp"

namespace aud::metadata {

// True if `text` contains at least one `[mm:ss.xx]`-or-`[mm:ss]` timestamp tag at the start of a
// line — the LRC format's defining feature.
[[nodiscard]] bool looksLikeLrc(const std::string& text) noexcept;

// Parses `text` as LRC: one or more `[mm:ss.xx]` tags (a line may carry more than one, meaning the
// same lyric repeats at multiple times) followed by the line's lyric text. Metadata tags like
// `[ar:...]`/`[ti:...]`/`[offset:...]` are recognised and skipped (they're not timed lines).
// Returns lines sorted by time. Call only after looksLikeLrc() returns true.
[[nodiscard]] std::vector<LyricLine> parseLrc(const std::string& text);

// Builds a Lyrics record from a plain-text unsynced field: LRC-detects it and, if it matches,
// produces a synced Lyrics; otherwise produces a single-line unsynced Lyrics with the whole text.
[[nodiscard]] Lyrics makeLyricsFromUnsyncedText(std::string text, std::string language, std::string description,
                                                 std::string sourceFormat);

}  // namespace aud::metadata

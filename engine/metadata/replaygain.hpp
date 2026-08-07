#pragma once

// ReplayGain parsing from all three sources named in M15, kept as independent, non-merged
// readings — see aud::metadata::ReplayGainInfo's comment for why sources aren't averaged/resolved.
// Never applies the gain (M02's rule, restated in M15): this is read-only reporting.

#include <optional>
#include <string>

#include "metadata.hpp"

namespace aud::metadata {

// Parses a REPLAYGAIN_TRACK_GAIN/REPLAYGAIN_ALBUM_GAIN-style value, e.g. "-6.30 dB" or "-6.30" —
// the trailing unit is optional and both forms appear in the wild.
[[nodiscard]] std::optional<double> parseReplayGainDb(const std::string& text);

// Parses a REPLAYGAIN_TRACK_PEAK/REPLAYGAIN_ALBUM_PEAK-style value, a plain linear float
// (e.g. "0.988244").
[[nodiscard]] std::optional<double> parseReplayGainPeak(const std::string& text);

// Best-effort iTunNORM decode (MP4's own scheme, a space-separated hex string, e.g.
// "00001990 00001990 ..."). iTunNORM's exact reference level/formula is not publicly documented by
// Apple and reverse-engineered write-ups disagree on details beyond the widely-used approximation
// applied here (gainDb = 10*log10(1000/value) on the first token) — reported as an estimate, not a
// certified figure; the raw string is always preserved in `unmapped` alongside it so a caller can
// re-derive their own interpretation.
[[nodiscard]] std::optional<double> parseItunNormTrackGainDb(const std::string& hexTokens);

}  // namespace aud::metadata

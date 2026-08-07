#pragma once

// ID3v2 frames declare one of four text encodings; MP4/Vorbis are always UTF-8. This is the one
// conversion layer everything funnels through — see M15 "Text encodings" and the mislabelled-
// Latin-1 heuristic below.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aud::metadata {

enum class TextEncoding : std::uint8_t {
    Latin1  = 0,  // ID3v2 declared value 0x00
    Utf16Bom = 1,  // 0x01 — UTF-16 with a byte-order mark (LE or BE)
    Utf16Be = 2,  // 0x02 — ID3v2.4 only: UTF-16BE, no BOM
    Utf8    = 3,  // 0x03 — ID3v2.4 only
};

// Converts `bytes` (declared to be in `encoding`) to a UTF-8 std::string.
//
// Mislabelling is endemic in the wild (M15): a frame declared Latin-1 that is actually valid UTF-8
// (multibyte sequences that would be mojibake as Latin-1) is heuristically reinterpreted as UTF-8;
// `wasMislabelled`, if non-null, is set to true when that heuristic fired so callers can surface a
// diagnostic.
std::string decodeToUtf8(std::span<const std::byte> bytes, TextEncoding encoding, bool* wasMislabelled = nullptr);

// True if `bytes` is well-formed UTF-8 and contains at least one multibyte sequence (a plain-ASCII
// string is "valid UTF-8" trivially and isn't evidence of anything — the heuristic only fires when
// there's an actual multibyte sequence to be confident about).
bool looksLikeMislabelledUtf8(std::span<const std::byte> bytes) noexcept;

// Splits a UTF-8 string on ID3v2.4 multi-value frames' 0x00 separators (M15 "Multi-value frames").
std::vector<std::string> splitNulSeparated(const std::string& utf8);

}  // namespace aud::metadata

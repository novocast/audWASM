#pragma once

// Shared byte-buffer builder primitives for M15's metadata tests — every test file that needs to
// hand-construct a tag (ID3v2 frames, FLAC metadata blocks, MP4 boxes, RIFF chunks) builds on top
// of these instead of repeating the same big/little-endian and syncsafe encoding by hand.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aud::metadata::test {

using Bytes = std::vector<std::byte>;

inline void appendByte(Bytes& out, std::uint8_t b) { out.push_back(static_cast<std::byte>(b)); }

inline void appendBytes(Bytes& out, std::initializer_list<std::uint8_t> bytes) {
    for (auto b : bytes) appendByte(out, b);
}

inline void appendStr(Bytes& out, const std::string& s) {
    for (char c : s) appendByte(out, static_cast<std::uint8_t>(c));
}

inline void appendNul(Bytes& out, std::size_t count = 1) {
    for (std::size_t i = 0; i < count; ++i) appendByte(out, 0);
}

inline void appendU32Be(Bytes& out, std::uint32_t v) {
    appendByte(out, static_cast<std::uint8_t>((v >> 24) & 0xFF));
    appendByte(out, static_cast<std::uint8_t>((v >> 16) & 0xFF));
    appendByte(out, static_cast<std::uint8_t>((v >> 8) & 0xFF));
    appendByte(out, static_cast<std::uint8_t>(v & 0xFF));
}

inline void appendU24Be(Bytes& out, std::uint32_t v) {
    appendByte(out, static_cast<std::uint8_t>((v >> 16) & 0xFF));
    appendByte(out, static_cast<std::uint8_t>((v >> 8) & 0xFF));
    appendByte(out, static_cast<std::uint8_t>(v & 0xFF));
}

inline void appendU16Be(Bytes& out, std::uint16_t v) {
    appendByte(out, static_cast<std::uint8_t>((v >> 8) & 0xFF));
    appendByte(out, static_cast<std::uint8_t>(v & 0xFF));
}

inline void appendU32Le(Bytes& out, std::uint32_t v) {
    appendByte(out, static_cast<std::uint8_t>(v & 0xFF));
    appendByte(out, static_cast<std::uint8_t>((v >> 8) & 0xFF));
    appendByte(out, static_cast<std::uint8_t>((v >> 16) & 0xFF));
    appendByte(out, static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

inline void appendU16Le(Bytes& out, std::uint16_t v) {
    appendByte(out, static_cast<std::uint8_t>(v & 0xFF));
    appendByte(out, static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

inline void appendSyncsafe32(Bytes& out, std::uint32_t v) {
    appendByte(out, static_cast<std::uint8_t>((v >> 21) & 0x7F));
    appendByte(out, static_cast<std::uint8_t>((v >> 14) & 0x7F));
    appendByte(out, static_cast<std::uint8_t>((v >> 7) & 0x7F));
    appendByte(out, static_cast<std::uint8_t>(v & 0x7F));
}

// UTF-16LE with BOM, NUL-terminated (double NUL) — for building ID3v2 encoding=1 frame bodies.
inline void appendUtf16LeWithBom(Bytes& out, const std::string& asciiText) {
    appendBytes(out, {0xFF, 0xFE});
    for (char c : asciiText) {
        appendByte(out, static_cast<std::uint8_t>(c));
        appendByte(out, 0);
    }
    appendNul(out, 2);
}

}  // namespace aud::metadata::test

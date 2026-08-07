#include "text_encoding.hpp"

namespace aud::metadata {

namespace {

// Appends `codepoint` to `out` as UTF-8.
void appendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string latin1ToUtf8(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (std::byte b : bytes) {
        const auto c = static_cast<std::uint8_t>(b);
        if (c == 0) break;  // ID3v1-style fields are often NUL-padded; ID3v2 Latin-1 frames may be too
        appendUtf8(out, c);
    }
    return out;
}

// Decodes a run of UTF-16 code units (already byte-order-corrected to host order) to UTF-8,
// stopping at a NUL terminator or the end of the buffer. Malformed surrogate pairs are replaced
// with U+FFFD rather than trusted — this data is attacker-controlled (M15's paranoia mandate).
std::string utf16UnitsToUtf8(const std::uint16_t* units, std::size_t count) {
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t unit = units[i];
        if (unit == 0) break;

        if (unit >= 0xD800 && unit <= 0xDBFF) {
            // High surrogate: needs a following low surrogate to be valid.
            if (i + 1 < count) {
                const std::uint16_t low = units[i + 1];
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    const std::uint32_t cp =
                        0x10000 + ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
                        (static_cast<std::uint32_t>(low) - 0xDC00);
                    appendUtf8(out, cp);
                    ++i;
                    continue;
                }
            }
            appendUtf8(out, 0xFFFD);  // unpaired high surrogate
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            appendUtf8(out, 0xFFFD);  // unpaired low surrogate
        } else {
            appendUtf8(out, unit);
        }
    }
    return out;
}

std::string utf16ToUtf8(std::span<const std::byte> bytes, bool bigEndian) {
    // Truncate to an even number of bytes; a dangling odd trailing byte is malformed input, not a
    // crash — silently drop it.
    const std::size_t unitCount = bytes.size() / 2;
    std::vector<std::uint16_t> units(unitCount);
    for (std::size_t i = 0; i < unitCount; ++i) {
        const auto b0 = static_cast<std::uint8_t>(bytes[2 * i]);
        const auto b1 = static_cast<std::uint8_t>(bytes[2 * i + 1]);
        units[i]      = bigEndian ? static_cast<std::uint16_t>((b0 << 8) | b1)
                                   : static_cast<std::uint16_t>((b1 << 8) | b0);
    }
    return utf16UnitsToUtf8(units.data(), units.size());
}

std::string utf16WithBomToUtf8(std::span<const std::byte> bytes) {
    if (bytes.size() >= 2) {
        const auto b0 = static_cast<std::uint8_t>(bytes[0]);
        const auto b1 = static_cast<std::uint8_t>(bytes[1]);
        if (b0 == 0xFF && b1 == 0xFE) return utf16ToUtf8(bytes.subspan(2), /*bigEndian=*/false);
        if (b0 == 0xFE && b1 == 0xFF) return utf16ToUtf8(bytes.subspan(2), /*bigEndian=*/true);
    }
    // No BOM despite being declared "UTF-16 with BOM" — endemic in the wild; assume little-endian
    // (the overwhelmingly common case, since it matches x86) rather than reject the frame.
    return utf16ToUtf8(bytes, /*bigEndian=*/false);
}

}  // namespace

bool looksLikeMislabelledUtf8(std::span<const std::byte> bytes) noexcept {
    bool          sawMultibyte = false;
    std::size_t   i            = 0;
    const std::size_t n        = bytes.size();

    while (i < n) {
        const auto b0 = static_cast<std::uint8_t>(bytes[i]);
        if (b0 == 0) break;  // NUL terminator/padding
        if (b0 < 0x80) {
            ++i;
            continue;
        }

        std::size_t extra;
        std::uint32_t minCodepoint;
        if ((b0 & 0xE0) == 0xC0) {
            extra        = 1;
            minCodepoint = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            extra        = 2;
            minCodepoint = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            extra        = 3;
            minCodepoint = 0x10000;
        } else {
            return false;  // not a valid UTF-8 lead byte at all
        }

        if (i + extra >= n) return false;  // truncated sequence

        std::uint32_t codepoint = b0 & (0x3F >> extra);
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<std::uint8_t>(bytes[i + k]);
            if ((cont & 0xC0) != 0x80) return false;  // not a continuation byte
            codepoint = (codepoint << 6) | (cont & 0x3F);
        }
        if (codepoint < minCodepoint) return false;         // overlong encoding — reject as not-UTF-8
        if (codepoint > 0x10FFFF) return false;              // outside Unicode range
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;  // surrogate half — not valid UTF-8

        sawMultibyte = true;
        i += extra + 1;
    }

    return sawMultibyte;
}

std::string decodeToUtf8(std::span<const std::byte> bytes, TextEncoding encoding, bool* wasMislabelled) {
    if (wasMislabelled != nullptr) *wasMislabelled = false;

    switch (encoding) {
        case TextEncoding::Latin1:
            if (looksLikeMislabelledUtf8(bytes)) {
                if (wasMislabelled != nullptr) *wasMislabelled = true;
                // Reinterpret verbatim as UTF-8, trimming at the first NUL like the Latin-1 path.
                std::size_t len = 0;
                while (len < bytes.size() && bytes[len] != std::byte{0}) ++len;
                const auto* data = reinterpret_cast<const char*>(bytes.data());
                return std::string(data, len);
            }
            return latin1ToUtf8(bytes);
        case TextEncoding::Utf16Bom:
            return utf16WithBomToUtf8(bytes);
        case TextEncoding::Utf16Be:
            return utf16ToUtf8(bytes, /*bigEndian=*/true);
        case TextEncoding::Utf8: {
            std::size_t len = 0;
            while (len < bytes.size() && bytes[len] != std::byte{0}) ++len;
            return std::string(reinterpret_cast<const char*>(bytes.data()), len);
        }
    }
    return {};
}

std::vector<std::string> splitNulSeparated(const std::string& utf8) {
    std::vector<std::string> parts;
    std::size_t              start = 0;
    for (std::size_t i = 0; i <= utf8.size(); ++i) {
        if (i == utf8.size() || utf8[i] == '\0') {
            if (i > start) parts.emplace_back(utf8.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.empty() && !utf8.empty()) parts.push_back(utf8);
    return parts;
}

}  // namespace aud::metadata

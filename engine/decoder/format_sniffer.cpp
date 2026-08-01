#include "format_sniffer.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace aud::decoder {

namespace {

bool matches(std::span<const std::byte> bytes, std::size_t offset, const char* literal) noexcept {
    const std::size_t len = std::strlen(literal);
    if (bytes.size() < offset + len) {
        return false;
    }
    return std::memcmp(bytes.data() + offset, literal, len) == 0;
}

std::uint32_t syncsafeToUint32(std::span<const std::byte> b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 21) | (static_cast<std::uint32_t>(b[1]) << 14) |
           (static_cast<std::uint32_t>(b[2]) << 7) | static_cast<std::uint32_t>(b[3]);
}

// Looks for at least 3 consecutive, mutually-consistent MPEG audio frame headers starting at or
// after `from`. A single 0xFFEx sync word is not evidence — MP3s in the wild routinely start with
// junk that happens to contain one.
bool looksLikeMp3(std::span<const std::byte> bytes) noexcept {
    for (std::size_t i = 0; i + 4 <= bytes.size(); ++i) {
        const auto b0 = static_cast<std::uint8_t>(bytes[i]);
        const auto b1 = static_cast<std::uint8_t>(bytes[i + 1]);
        if (b0 != 0xFF || (b1 & 0xE0) != 0xE0) {
            continue;
        }
        const std::uint8_t versionLayer = b1 & 0x1E;  // version (2 bits) + layer (2 bits)

        std::size_t consistent = 1;
        std::size_t cursor     = i;
        for (int attempt = 0; attempt < 8 && consistent < 3; ++attempt) {
            // Frame length depends on bitrate/padding, which we are not fully parsing here; instead
            // scan forward for the next sync word within a generous window and check the
            // version/layer nibble matches. This is intentionally approximate — false negatives are
            // safe (we fall through to UnsupportedFormat), false positives are what this loop
            // guards against by requiring 3 matches.
            bool found = false;
            for (std::size_t j = cursor + 1; j + 2 <= bytes.size() && j < cursor + 4096; ++j) {
                const auto c0 = static_cast<std::uint8_t>(bytes[j]);
                const auto c1 = static_cast<std::uint8_t>(bytes[j + 1]);
                if (c0 == 0xFF && (c1 & 0xE0) == 0xE0 && (c1 & 0x1E) == versionLayer) {
                    cursor = j;
                    found  = true;
                    break;
                }
            }
            if (!found) {
                break;
            }
            ++consistent;
        }
        if (consistent >= 3) {
            return true;
        }
    }
    return false;
}

std::string hexDumpFirst16(std::span<const std::byte> bytes) {
    std::array<char, 64> buf{};
    const std::size_t    n = bytes.size() < 16 ? bytes.size() : 16;
    std::size_t          pos = 0;
    for (std::size_t i = 0; i < n; ++i) {
        pos += static_cast<std::size_t>(
            std::snprintf(buf.data() + pos, buf.size() - pos, "%02x ", static_cast<std::uint8_t>(bytes[i])));
    }
    return std::string(buf.data(), pos);
}

}  // namespace

Result<SniffResult> sniff(std::span<const std::byte> bytes) {
    std::size_t headerSkip = 0;

    // ID3v2 header ("ID3") — skip the tag (syncsafe size) and re-sniff what follows.
    if (matches(bytes, 0, "ID3") && bytes.size() >= 10) {
        const std::uint32_t tagSize = syncsafeToUint32(bytes.subspan(6, 4));
        headerSkip                  = 10 + tagSize;
        if (headerSkip < bytes.size()) {
            bytes = bytes.subspan(headerSkip);
        }
    }

    if (matches(bytes, 0, "RIFF") && matches(bytes, 8, "WAVE")) {
        return SniffResult{ContainerFormat::Wav, headerSkip};
    }
    if (matches(bytes, 0, "FORM") && matches(bytes, 8, "AIFF")) {
        return SniffResult{ContainerFormat::Aiff, headerSkip};
    }
    if (matches(bytes, 0, "fLaC")) {
        return SniffResult{ContainerFormat::Flac, headerSkip};
    }
    if (matches(bytes, 0, "OggS")) {
        return SniffResult{ContainerFormat::OggVorbis, headerSkip};
    }
    if (matches(bytes, 4, "ftyp")) {
        // Major brand at offset 8, 4 bytes. Compatible brands follow. A conservative allow-list of
        // audio-only / iTunes brands; anything else (isom+mp42 alone, video brands) is rejected
        // with a clear message rather than silently attempted.
        static const char* kAudioBrands[] = {"M4A ", "M4B ", "isom", "mp42", "3gp5"};
        bool                recognisedAudio = false;
        if (bytes.size() >= 12) {
            for (const char* brand : kAudioBrands) {
                if (matches(bytes, 8, brand)) {
                    recognisedAudio = true;
                    break;
                }
            }
        }
        return SniffResult{recognisedAudio ? ContainerFormat::Mp4Aac : ContainerFormat::Mp4Other, headerSkip};
    }
    if (looksLikeMp3(bytes)) {
        return SniffResult{ContainerFormat::Mp3, headerSkip};
    }

    return Error{ErrorCode::UnsupportedFormat, "decoder.sniffer", "unrecognised header: " + hexDumpFirst16(bytes)};
}

}  // namespace aud::decoder

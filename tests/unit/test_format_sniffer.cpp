#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "../../engine/decoder/format_sniffer.hpp"

using namespace aud::decoder;

namespace {

// Builds a byte vector from raw literal bytes (may contain embedded NULs — `length` is explicit,
// never derived from strlen), optionally zero-padded to `padTo`.
std::vector<std::byte> bytesOf(const char* literal, std::size_t length, std::size_t padTo = 0) {
    std::vector<std::byte> out(length);
    std::memcpy(out.data(), literal, length);
    out.resize(padTo > out.size() ? padTo : out.size());
    return out;
}

}  // namespace

TEST_CASE("sniff recognises WAV via RIFF....WAVE", "[format_sniffer]") {
    auto bytes  = bytesOf("RIFF\0\0\0\0WAVEfmt ", 16, 16);
    auto result = sniff(bytes);
    REQUIRE(result.has_value());
    REQUIRE(result.value().format == ContainerFormat::Wav);
}

TEST_CASE("sniff recognises FLAC via fLaC magic", "[format_sniffer]") {
    auto bytes  = bytesOf("fLaC\x00\x00\x00\x22", 8, 32);
    auto result = sniff(bytes);
    REQUIRE(result.has_value());
    REQUIRE(result.value().format == ContainerFormat::Flac);
}

TEST_CASE("sniff recognises Ogg via OggS magic", "[format_sniffer]") {
    auto bytes  = bytesOf("OggS", 4, 32);
    auto result = sniff(bytes);
    REQUIRE(result.has_value());
    REQUIRE(result.value().format == ContainerFormat::OggVorbis);
}

TEST_CASE("sniff skips a leading ID3v2 tag before re-sniffing", "[format_sniffer]") {
    // "ID3" + major(1) + minor(1) + flags(1) + syncsafe size (4, = 10) = 10-byte header.
    std::vector<std::byte> bytes = bytesOf("ID3\x03\x00\x00", 6);
    bytes.push_back(static_cast<std::byte>(0x00));
    bytes.push_back(static_cast<std::byte>(0x00));
    bytes.push_back(static_cast<std::byte>(0x00));
    bytes.push_back(static_cast<std::byte>(0x0A));
    std::vector<std::byte> tagBody(10, std::byte{0});
    bytes.insert(bytes.end(), tagBody.begin(), tagBody.end());
    auto wav = bytesOf("RIFF\0\0\0\0WAVEfmt ", 16, 16);
    bytes.insert(bytes.end(), wav.begin(), wav.end());

    auto result = sniff(bytes);
    REQUIRE(result.has_value());
    REQUIRE(result.value().format == ContainerFormat::Wav);
    REQUIRE(result.value().headerBytesConsumed == 20);
}

TEST_CASE("sniff rejects a video-brand MP4 with a clear UnsupportedFormat error", "[format_sniffer]") {
    // "ftyp" at offset 4, major brand "isom" would be treated as audio-capable in our conservative
    // allow-list, so use a brand that is NOT in it to exercise the Mp4Other path.
    auto bytes  = bytesOf("\x00\x00\x00\x18" "ftypmp41", 12, 32);
    auto result = sniff(bytes);
    REQUIRE(result.has_value());
    REQUIRE(result.value().format == ContainerFormat::Mp4Other);
}

TEST_CASE("sniff returns UnsupportedFormat with a hex dump for garbage input", "[format_sniffer]") {
    std::vector<std::byte> garbage(64, std::byte{0xAB});
    auto                   result = sniff(garbage);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == aud::ErrorCode::UnsupportedFormat);
    REQUIRE(result.error().detail.find("ab") != std::string::npos);
}

TEST_CASE("sniff does not false-positive MP3 on a single stray 0xFF byte", "[format_sniffer]") {
    std::vector<std::byte> bytes(64, std::byte{0x00});
    bytes[10] = std::byte{0xFF};
    bytes[11] = std::byte{0xE0};  // one lone sync word, not 3 consistent frames
    auto result = sniff(bytes);
    REQUIRE_FALSE(result.has_value());
}

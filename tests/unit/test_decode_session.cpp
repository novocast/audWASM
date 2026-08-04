#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../engine/decoder/decode_session.hpp"

using namespace aud::decoder;

namespace {

// A minimal valid WAV: RIFF/WAVE, fmt chunk (PCM, mono, 8kHz, 16-bit), data chunk with `frames`
// silent frames.
std::vector<std::byte> buildMinimalWav(std::uint32_t frames) {
    const std::uint32_t     dataBytes = frames * 2;  // mono, 16-bit
    std::vector<std::byte>  buf(44 + dataBytes);
    auto*                   p = reinterpret_cast<unsigned char*>(buf.data());
    std::size_t             off = 0;

    auto writeString = [&](const char* s) {
        std::memcpy(p + off, s, std::strlen(s));
        off += std::strlen(s);
    };
    auto write32 = [&](std::uint32_t v) {
        std::memcpy(p + off, &v, 4);
        off += 4;
    };
    auto write16 = [&](std::uint16_t v) {
        std::memcpy(p + off, &v, 2);
        off += 2;
    };

    writeString("RIFF");
    write32(36 + dataBytes);
    writeString("WAVE");
    writeString("fmt ");
    write32(16);      // fmt chunk size
    write16(1);       // PCM
    write16(1);       // mono
    write32(8000);     // sample rate
    write32(16000);    // byte rate
    write16(2);        // block align
    write16(16);       // bits per sample
    writeString("data");
    write32(dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        write16(0);
    }

    return buf;
}

}  // namespace

TEST_CASE("DecodeSession decodes a minimal in-memory WAV fed in a single call", "[decode_session]") {
    auto bytes = buildMinimalWav(4);

    auto sessionResult = DecodeSession::create(bytes);
    REQUIRE(sessionResult.has_value());
    auto session = std::move(sessionResult).value();

    REQUIRE(session.feed(bytes).has_value());
    REQUIRE(session.finish().has_value());

    auto infoResult = session.streamInfo();
    REQUIRE(infoResult.has_value());
    CHECK(infoResult.value().sampleRate == 8000);
    CHECK(infoResult.value().channels == 1);

    REQUIRE(session.buffer() != nullptr);
    // Regression test: dr_wav validates its declared data-chunk size against the real stream
    // length via a SEEK_END query. Mishandling SEEK_END (treating it as SEEK_CUR) makes dr_wav
    // believe the file ends at the parse cursor, right after the header, and it silently clamps
    // the frame count to 0 — feed()/finish() still report success. See wav_decoder.cpp's onSeek.
    CHECK(session.buffer()->frameCount() == 4);
}

TEST_CASE("DecodeSession decodes a minimal WAV fed across multiple feed() calls", "[decode_session]") {
    auto bytes = buildMinimalWav(4);

    auto sessionResult = DecodeSession::create(bytes);
    REQUIRE(sessionResult.has_value());
    auto session = std::move(sessionResult).value();

    // Split into a header-only slice and a data-only slice, mirroring progressive JS-side reads.
    constexpr std::size_t headerSize = 44;
    std::span<const std::byte> header(bytes.data(), headerSize);
    std::span<const std::byte> data(bytes.data() + headerSize, bytes.size() - headerSize);

    REQUIRE(session.feed(header).has_value());
    REQUIRE(session.feed(data).has_value());
    REQUIRE(session.finish().has_value());

    REQUIRE(session.buffer() != nullptr);
    CHECK(session.buffer()->frameCount() == 4);
}

TEST_CASE("DecodeSession clamps to the actual byte count when the data chunk is shorter than declared",
          "[decode_session]") {
    auto bytes = buildMinimalWav(4);
    bytes.resize(bytes.size() - 4);  // drop the last two frames' worth of data bytes; header still says 4

    auto sessionResult = DecodeSession::create(bytes);
    REQUIRE(sessionResult.has_value());
    auto session = std::move(sessionResult).value();

    REQUIRE(session.feed(bytes).has_value());
    REQUIRE(session.finish().has_value());

    REQUIRE(session.buffer() != nullptr);
    CHECK(session.buffer()->frameCount() == 2);
}

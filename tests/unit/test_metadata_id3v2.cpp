// M15: syncsafe-vs-plain frame sizes (the classic v2.3/v2.4 bug), unsynchronisation at both tag
// and frame level, and a couple of binary frames (RVA2, TXXX replaygain).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../engine/metadata/id3v2.hpp"
#include "metadata_test_bytes.hpp"

using namespace aud::metadata;
using namespace aud::metadata::test;

namespace {

// Builds a minimal text frame body: [encoding=0][Latin-1 text, no terminator needed at frame end].
Bytes latin1TextFrameBody(const std::string& text) {
    Bytes body;
    appendByte(body, 0);
    appendStr(body, text);
    return body;
}

}  // namespace

TEST_CASE("id3v2: v2.3 frame sizes are plain 32-bit big-endian, not syncsafe", "[metadata][id3v2]") {
    // A frame body long enough (>127 bytes) that a syncsafe misinterpretation of its *plain* size
    // would compute a different (wrong) length — proving the parser used the right rule for v2.3.
    const std::string longTitle(200, 'x');
    Bytes              frameBody = latin1TextFrameBody(longTitle);

    Bytes tag;
    appendStr(tag, "ID3");
    appendBytes(tag, {3, 0});  // major version 3, revision 0
    appendByte(tag, 0);        // flags: no unsync, no extended header
    const std::uint32_t frameHeaderAndBody = 10 + static_cast<std::uint32_t>(frameBody.size());
    appendSyncsafe32(tag, frameHeaderAndBody);  // tag size IS syncsafe in the outer header, always

    appendStr(tag, "TIT2");
    appendU32Be(tag, static_cast<std::uint32_t>(frameBody.size()));  // plain BE, v2.3 rule
    appendU16Be(tag, 0);                                             // frame flags
    tag.insert(tag.end(), frameBody.begin(), frameBody.end());

    Id3v2ParseResult result = parseId3v2(tag);
    REQUIRE(result.present);
    REQUIRE(result.tags.title.has_value());
    CHECK(*result.tags.title == longTitle);
}

TEST_CASE("id3v2: v2.4 frame sizes are syncsafe", "[metadata][id3v2]") {
    const std::string longTitle(200, 'y');
    Bytes              frameBody = latin1TextFrameBody(longTitle);

    Bytes tag;
    appendStr(tag, "ID3");
    appendBytes(tag, {4, 0});
    appendByte(tag, 0);
    appendSyncsafe32(tag, 10 + static_cast<std::uint32_t>(frameBody.size()));

    appendStr(tag, "TIT2");
    appendSyncsafe32(tag, static_cast<std::uint32_t>(frameBody.size()));  // syncsafe, v2.4 rule
    appendU16Be(tag, 0);
    tag.insert(tag.end(), frameBody.begin(), frameBody.end());

    Id3v2ParseResult result = parseId3v2(tag);
    REQUIRE(result.present);
    REQUIRE(result.tags.title.has_value());
    CHECK(*result.tags.title == longTitle);
}

TEST_CASE("id3v2: tag-level unsynchronisation is undone before frame parsing (v2.3 style)", "[metadata][id3v2]") {
    // Logical title bytes are 'A', 0xFF, 0xE0, 'B' — 0xFF followed by a byte with its top 3 bits
    // set is exactly the "false MPEG sync" pattern the unsync scheme exists to break, so an
    // encoder would have inserted a 0x00 after the 0xFF. The *stored* frame therefore contains
    // 'A',0xFF,0x00,0xE0,'B'; decoding must collapse it back to the 4-byte logical form. (Using a
    // literal 0x00 as the "real" byte here would be indistinguishable from Latin-1's own NUL
    // termination, so 0xE0 is used instead — it also happens to be the other half of the false-
    // sync pattern that motivated unsync in the first place.)
    Bytes storedText;
    appendStr(storedText, "A");
    appendByte(storedText, 0xFF);
    appendByte(storedText, 0x00);  // inserted by unsync
    appendByte(storedText, 0xE0);  // the real byte that followed 0xFF
    appendStr(storedText, "B");

    Bytes frameBody;
    appendByte(frameBody, 0);  // encoding = Latin-1
    frameBody.insert(frameBody.end(), storedText.begin(), storedText.end());

    // Tag-level unsync is undone once, over the *whole* post-header payload, before any frame is
    // parsed — so the frame size field describes the frame's LOGICAL (post-desync) length (5:
    // encoding byte + 'A' + 0xFF + 0xE0 + 'B'), even though 6 STORED bytes are physically present
    // on disk. (Contrast with the per-frame-unsync test below, where the size field must describe
    // the STORED length instead, since no global desync pass has happened yet to find the frame.)
    const std::uint32_t logicalFrameBodySize = static_cast<std::uint32_t>(frameBody.size()) - 1;

    Bytes tag;
    appendStr(tag, "ID3");
    appendBytes(tag, {3, 0});
    appendByte(tag, 0x80);  // unsynchronisation flag set
    appendSyncsafe32(tag, 10 + static_cast<std::uint32_t>(frameBody.size()));  // physical/stored byte count

    appendStr(tag, "TIT2");
    appendU32Be(tag, logicalFrameBodySize);
    appendU16Be(tag, 0);
    tag.insert(tag.end(), frameBody.begin(), frameBody.end());

    Id3v2ParseResult result = parseId3v2(tag);
    REQUIRE(result.present);
    REQUIRE(result.tags.title.has_value());

    // Latin-1 -> UTF-8 turns each of the two non-ASCII bytes into a 2-byte UTF-8 sequence, so the
    // correctly-desynced 4 logical bytes ('A', 0xFF, 0xE0, 'B') decode to 6 UTF-8 bytes. Getting
    // unsync wrong would either lose the trailing "0xE0B" (if the inserted 0x00 isn't dropped, the
    // *next* frame-size/frame-id bytes get misread — but concretely here it would still show up
    // short) or corrupt it outright — either way the length/content check below catches it.
    CHECK(*result.tags.title == "A\xC3\xBF\xC3\xA0" "B");
}

TEST_CASE("id3v2: per-frame unsynchronisation (v2.4 style) applies only to the flagged frame", "[metadata][id3v2]") {
    Bytes unsyncedFrameBody;
    appendByte(unsyncedFrameBody, 0);
    appendStr(unsyncedFrameBody, "A");
    appendByte(unsyncedFrameBody, 0xFF);
    appendByte(unsyncedFrameBody, 0x00);  // inserted
    appendByte(unsyncedFrameBody, 0xE0);  // real trailing byte
    appendStr(unsyncedFrameBody, "B");

    Bytes plainFrameBody;
    appendByte(plainFrameBody, 0);
    appendStr(plainFrameBody, "PlainArtist");

    Bytes tag;
    appendStr(tag, "ID3");
    appendBytes(tag, {4, 0});
    appendByte(tag, 0);  // tag-level unsync NOT set
    const std::uint32_t totalFrames =
        (10 + static_cast<std::uint32_t>(unsyncedFrameBody.size())) + (10 + static_cast<std::uint32_t>(plainFrameBody.size()));
    appendSyncsafe32(tag, totalFrames);

    appendStr(tag, "TIT2");
    appendSyncsafe32(tag, static_cast<std::uint32_t>(unsyncedFrameBody.size()));
    appendU16Be(tag, 0x0002);  // v2.4 format-flags: unsynchronisation bit set for this frame only
    tag.insert(tag.end(), unsyncedFrameBody.begin(), unsyncedFrameBody.end());

    appendStr(tag, "TPE1");
    appendSyncsafe32(tag, static_cast<std::uint32_t>(plainFrameBody.size()));
    appendU16Be(tag, 0);
    tag.insert(tag.end(), plainFrameBody.begin(), plainFrameBody.end());

    Id3v2ParseResult result = parseId3v2(tag);
    REQUIRE(result.present);
    REQUIRE(result.tags.title.has_value());
    CHECK(*result.tags.title == "A\xC3\xBF\xC3\xA0" "B");
    REQUIRE(result.tags.artist.has_value());
    CHECK(*result.tags.artist == "PlainArtist");
}

TEST_CASE("id3v2: TXXX REPLAYGAIN_TRACK_GAIN is parsed", "[metadata][id3v2][replaygain]") {
    Bytes frameBody;
    appendByte(frameBody, 0);  // encoding
    appendStr(frameBody, "REPLAYGAIN_TRACK_GAIN");
    appendByte(frameBody, 0);  // NUL separator
    appendStr(frameBody, "-6.30 dB");

    Bytes tag;
    appendStr(tag, "ID3");
    appendBytes(tag, {3, 0});
    appendByte(tag, 0);
    appendSyncsafe32(tag, 10 + static_cast<std::uint32_t>(frameBody.size()));
    appendStr(tag, "TXXX");
    appendU32Be(tag, static_cast<std::uint32_t>(frameBody.size()));
    appendU16Be(tag, 0);
    tag.insert(tag.end(), frameBody.begin(), frameBody.end());

    Id3v2ParseResult result = parseId3v2(tag);
    REQUIRE(result.present);
    REQUIRE(result.tags.replayGainSources.size() == 1);
    REQUIRE(result.tags.replayGainSources[0].trackGainDb.has_value());
    CHECK(*result.tags.replayGainSources[0].trackGainDb == Catch::Approx(-6.30).margin(0.001));
}

TEST_CASE("id3v2: a frame declaring a size larger than the remaining tag is rejected safely", "[metadata][id3v2]") {
    Bytes tag;
    appendStr(tag, "ID3");
    appendBytes(tag, {4, 0});
    appendByte(tag, 0);
    appendSyncsafe32(tag, 20);  // small declared tag size

    appendStr(tag, "APIC");
    appendSyncsafe32(tag, 200u * 1024u * 1024u);  // 200MB — wildly larger than this tiny tag
    appendU16Be(tag, 0);
    // No picture bytes actually follow — a real 200MB frame was never written to this ~24-byte
    // buffer, proving the parser never had to allocate/read that much to reject it.

    Id3v2ParseResult result = parseId3v2(tag);
    REQUIRE(result.present);
    CHECK(result.tags.pictures.empty());
    CHECK_FALSE(result.tags.diagnostics.empty());
}

// M15: Vorbis comments, the FLAC metadata-block walker (STREAMINFO/VORBIS_COMMENT/PICTURE/
// CUESHEET), and the "200MB declared picture length in a small file is rejected without
// allocating" acceptance criterion.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../engine/metadata/cue.hpp"
#include "../../engine/metadata/vorbis_comment.hpp"
#include "metadata_test_bytes.hpp"

using namespace aud::metadata;
using namespace aud::metadata::test;

namespace {

Bytes vorbisCommentPayload(const std::string& vendor, const std::vector<std::string>& comments) {
    Bytes payload;
    appendU32Le(payload, static_cast<std::uint32_t>(vendor.size()));
    appendStr(payload, vendor);
    appendU32Le(payload, static_cast<std::uint32_t>(comments.size()));
    for (const auto& c : comments) {
        appendU32Le(payload, static_cast<std::uint32_t>(c.size()));
        appendStr(payload, c);
    }
    return payload;
}

Bytes flacMetadataBlock(std::uint8_t blockType, bool isLast, const Bytes& body) {
    Bytes out;
    appendByte(out, static_cast<std::uint8_t>((isLast ? 0x80 : 0x00) | (blockType & 0x7F)));
    appendU24Be(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

TEST_CASE("vorbis_comment: key=value pairs map onto common fields, case-insensitively", "[metadata][vorbis]") {
    Bytes payload = vorbisCommentPayload("test encoder 1.0", {"TITLE=My Song", "artist=My Artist",
                                                                "REPLAYGAIN_TRACK_GAIN=-4.50 dB"});
    TagSet tags = parseVorbisCommentPayload(payload, "vorbis");

    REQUIRE(tags.title.has_value());
    CHECK(*tags.title == "My Song");
    REQUIRE(tags.artist.has_value());
    CHECK(*tags.artist == "My Artist");
    REQUIRE(tags.replayGainSources.size() == 1);
    REQUIRE(tags.replayGainSources[0].trackGainDb.has_value());
    CHECK(*tags.replayGainSources[0].trackGainDb == Catch::Approx(-4.50).margin(0.001));
}

TEST_CASE("vorbis_comment: an entry with no '=' is ignored, not treated as an error", "[metadata][vorbis]") {
    Bytes  payload = vorbisCommentPayload("enc", {"NOT-A-KEYVALUE-PAIR", "TITLE=Real Title"});
    TagSet tags     = parseVorbisCommentPayload(payload, "vorbis");
    REQUIRE(tags.title.has_value());
    CHECK(*tags.title == "Real Title");
}

TEST_CASE("flac: STREAMINFO sample rate and VORBIS_COMMENT are both read", "[metadata][flac]") {
    Bytes streamInfoBody(34, std::byte{0});
    // Sample rate occupies the top 20 bits of bytes[10..18); 44100 = 0x0AC44.
    const std::uint32_t sampleRate = 44100;
    streamInfoBody[10]              = static_cast<std::byte>((sampleRate >> 12) & 0xFF);
    streamInfoBody[11]              = static_cast<std::byte>((sampleRate >> 4) & 0xFF);
    streamInfoBody[12]              = static_cast<std::byte>((sampleRate << 4) & 0xF0);

    Bytes commentBody = vorbisCommentPayload("flac enc", {"ALBUM=My Album"});

    Bytes file;
    appendStr(file, "fLaC");
    Bytes streamInfoBlock = flacMetadataBlock(0, /*isLast=*/false, streamInfoBody);
    Bytes commentBlock     = flacMetadataBlock(4, /*isLast=*/true, commentBody);
    file.insert(file.end(), streamInfoBlock.begin(), streamInfoBlock.end());
    file.insert(file.end(), commentBlock.begin(), commentBlock.end());

    FlacParseResult result = parseFlacMetadataBlocks(file);
    REQUIRE(result.present);
    CHECK(result.sampleRate == sampleRate);
    REQUIRE(result.tags.album.has_value());
    CHECK(*result.tags.album == "My Album");
}

TEST_CASE("flac: a PICTURE block claiming a 200MB image in a tiny file is rejected without allocating",
          "[metadata][flac][pictures]") {
    Bytes pictureBody;
    appendU32Be(pictureBody, 3);         // picture type: front cover
    appendU32Be(pictureBody, 10);        // mime length
    appendStr(pictureBody, "image/jpeg");
    // NB: "image/jpeg" is 10 chars, matches mime length above.
    appendU32Be(pictureBody, 0);         // description length
    appendU32Be(pictureBody, 0);         // width
    appendU32Be(pictureBody, 0);         // height
    appendU32Be(pictureBody, 0);         // color depth
    appendU32Be(pictureBody, 0);         // colors used
    appendU32Be(pictureBody, 200u * 1024u * 1024u);  // declared data length: 200MB
    // No actual picture bytes follow — the block is only a few dozen bytes long in total.

    Bytes file;
    appendStr(file, "fLaC");
    Bytes pictureBlock = flacMetadataBlock(6, /*isLast=*/true, pictureBody);
    file.insert(file.end(), pictureBlock.begin(), pictureBlock.end());

    REQUIRE(file.size() < 1024);  // this whole "file" is well under 1KB

    FlacParseResult result = parseFlacMetadataBlocks(file);
    REQUIRE(result.present);
    CHECK(result.tags.pictures.empty());  // rejected: declared length overruns the block
}

TEST_CASE("flac: CUESHEET track index points convert sample offsets to seconds", "[metadata][flac][cue]") {
    Bytes cueBody(128 + 8 + 1 + 258 + 1, std::byte{0});  // fixed header, zeroed (catalog/lead-in/flags/reserved)
    cueBody.back() = std::byte{1};                        // track count = 1

    // One track: offset(8 BE)=0, track number=1, ISRC(12), reserved(14: flag byte + 13), index count=1
    Bytes track;
    for (int i = 0; i < 8; ++i) appendByte(track, 0);  // track offset = 0 samples
    appendByte(track, 1);                               // track number
    for (int i = 0; i < 12; ++i) appendByte(track, 0);  // ISRC
    for (int i = 0; i < 14; ++i) appendByte(track, 0);  // flags + reserved
    appendByte(track, 1);                               // index point count = 1
    // One index point: offset(8 BE) = 44100 samples, index number = 1, reserved(3)
    appendU32Be(track, 0);
    appendU32Be(track, 44100);
    appendByte(track, 1);
    for (int i = 0; i < 3; ++i) appendByte(track, 0);

    cueBody.insert(cueBody.end(), track.begin(), track.end());

    auto cuePoints = parseFlacCueSheet(cueBody, 44100);
    REQUIRE(cuePoints.size() == 1);
    CHECK(cuePoints[0].timeSeconds == Catch::Approx(1.0));
}

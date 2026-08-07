// M15: WAV's RIFF LIST/INFO chunk, the BWF `bext` chunk, and the native `cue ` + LIST/adtl `labl`
// cue-point mechanism.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../engine/metadata/riff.hpp"
#include "metadata_test_bytes.hpp"

using namespace aud::metadata;
using namespace aud::metadata::test;

namespace {

Bytes riffChunk(const std::string& id, const Bytes& body) {
    Bytes out;
    appendStr(out, id);
    appendU32Le(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() % 2 != 0) appendByte(out, 0);  // pad byte, not counted in the chunk size
    return out;
}

Bytes fixedField(const std::string& text, std::size_t width) {
    Bytes out;
    appendStr(out, text);
    appendNul(out, width - text.size());
    return out;
}

}  // namespace

TEST_CASE("riff: LIST/INFO fields map onto common fields", "[metadata][riff]") {
    Bytes infoBody;
    appendStr(infoBody, "INFO");
    Bytes inam = riffChunk("INAM", []{ Bytes b; appendStr(b, "Test Title"); return b; }());
    infoBody.insert(infoBody.end(), inam.begin(), inam.end());
    Bytes iart = riffChunk("IART", []{ Bytes b; appendStr(b, "Test Artist"); return b; }());
    infoBody.insert(infoBody.end(), iart.begin(), iart.end());

    Bytes fmtBody;
    appendU16Le(fmtBody, 1);       // PCM
    appendU16Le(fmtBody, 2);       // channels
    appendU32Le(fmtBody, 44100);   // sample rate
    appendU32Le(fmtBody, 176400);  // byte rate
    appendU16Le(fmtBody, 4);       // block align
    appendU16Le(fmtBody, 16);      // bits per sample

    Bytes file;
    appendStr(file, "RIFF");
    appendU32Le(file, 0);  // patched below
    appendStr(file, "WAVE");
    Bytes fmtChunk  = riffChunk("fmt ", fmtBody);
    Bytes listChunk = riffChunk("LIST", infoBody);
    file.insert(file.end(), fmtChunk.begin(), fmtChunk.end());
    file.insert(file.end(), listChunk.begin(), listChunk.end());
    const std::uint32_t riffSize = static_cast<std::uint32_t>(file.size() - 8);
    file[4] = static_cast<std::byte>(riffSize & 0xFF);
    file[5] = static_cast<std::byte>((riffSize >> 8) & 0xFF);
    file[6] = static_cast<std::byte>((riffSize >> 16) & 0xFF);
    file[7] = static_cast<std::byte>((riffSize >> 24) & 0xFF);

    RiffParseResult result = parseRiff(file);
    REQUIRE(result.present);
    REQUIRE(result.tags.title.has_value());
    CHECK(*result.tags.title == "Test Title");
    REQUIRE(result.tags.artist.has_value());
    CHECK(*result.tags.artist == "Test Artist");
}

TEST_CASE("riff: bext chunk fields (originator, time reference, version-gated loudness)", "[metadata][riff][bext]") {
    // Fixed BWF layout, built field-by-field at its exact offsets (see riff.cpp's parseBext):
    // description[256], originator[32], originatorRef[32], date[10], time[8], timeRefLow(4LE),
    // timeRefHigh(4LE), version(2LE, read as the low 16 bits of a 4-byte LE read), UMID[64],
    // 5x loudness int16, 180 reserved, then coding history (variable, omitted here).
    Bytes bext;
    auto  description   = fixedField("A description", 256);
    auto  originator     = fixedField("An originator", 32);
    auto  originatorRef  = fixedField("REF123", 32);
    auto  date            = fixedField("2024-01-02", 10);
    auto  time             = fixedField("12:00:00", 8);
    bext.insert(bext.end(), description.begin(), description.end());
    bext.insert(bext.end(), originator.begin(), originator.end());
    bext.insert(bext.end(), originatorRef.begin(), originatorRef.end());
    bext.insert(bext.end(), date.begin(), date.end());
    bext.insert(bext.end(), time.begin(), time.end());
    appendU32Le(bext, 12345);  // time reference low
    appendU32Le(bext, 0);      // time reference high
    appendU16Le(bext, 2);      // version 2
    REQUIRE(bext.size() == 348);
    bext.resize(602, std::byte{0});  // pad out to the full fixed BWF layout (UMID + reserved loudness fields)

    Bytes file;
    appendStr(file, "RIFF");
    appendU32Le(file, 0);
    appendStr(file, "WAVE");
    Bytes bextChunk = riffChunk("bext", bext);
    file.insert(file.end(), bextChunk.begin(), bextChunk.end());
    const std::uint32_t riffSize = static_cast<std::uint32_t>(file.size() - 8);
    file[4] = static_cast<std::byte>(riffSize & 0xFF);
    file[5] = static_cast<std::byte>((riffSize >> 8) & 0xFF);
    file[6] = static_cast<std::byte>((riffSize >> 16) & 0xFF);
    file[7] = static_cast<std::byte>((riffSize >> 24) & 0xFF);

    RiffParseResult parsed = parseRiff(file);
    REQUIRE(parsed.present);
    CHECK(parsed.tags.broadcast.present);
    CHECK(parsed.tags.broadcast.originator == "An originator");
    CHECK(parsed.tags.broadcast.timeReference == 12345);
}

TEST_CASE("riff: native cue chunk + adtl labl produce labelled cue points", "[metadata][riff][cue]") {
    Bytes fmtBody;
    appendU16Le(fmtBody, 1);
    appendU16Le(fmtBody, 1);
    appendU32Le(fmtBody, 44100);
    appendU32Le(fmtBody, 88200);
    appendU16Le(fmtBody, 2);
    appendU16Le(fmtBody, 16);

    Bytes cueBody;
    appendU32Le(cueBody, 1);  // one cue point
    appendU32Le(cueBody, 42);       // id
    appendU32Le(cueBody, 0);        // position (unused by the parser)
    appendStr(cueBody, "data");     // data chunk id
    appendU32Le(cueBody, 0);        // chunk start
    appendU32Le(cueBody, 0);        // block start
    appendU32Le(cueBody, 22050);    // sample offset -> 0.5s at 44100Hz

    Bytes lablBody;
    appendU32Le(lablBody, 42);
    appendStr(lablBody, "Verse");
    appendByte(lablBody, 0);

    Bytes adtlBody;
    appendStr(adtlBody, "adtl");
    Bytes lablChunk = riffChunk("labl", lablBody);
    adtlBody.insert(adtlBody.end(), lablChunk.begin(), lablChunk.end());

    Bytes file;
    appendStr(file, "RIFF");
    appendU32Le(file, 0);
    appendStr(file, "WAVE");
    Bytes fmtChunk  = riffChunk("fmt ", fmtBody);
    Bytes cueChunk   = riffChunk("cue ", cueBody);
    Bytes listChunk  = riffChunk("LIST", adtlBody);
    file.insert(file.end(), fmtChunk.begin(), fmtChunk.end());
    file.insert(file.end(), cueChunk.begin(), cueChunk.end());
    file.insert(file.end(), listChunk.begin(), listChunk.end());
    const std::uint32_t riffSize = static_cast<std::uint32_t>(file.size() - 8);
    file[4] = static_cast<std::byte>(riffSize & 0xFF);
    file[5] = static_cast<std::byte>((riffSize >> 8) & 0xFF);
    file[6] = static_cast<std::byte>((riffSize >> 16) & 0xFF);
    file[7] = static_cast<std::byte>((riffSize >> 24) & 0xFF);

    RiffParseResult result = parseRiff(file);
    REQUIRE(result.present);
    REQUIRE(result.tags.cuePoints.size() == 1);
    CHECK(result.tags.cuePoints[0].label == "Verse");
    CHECK(result.tags.cuePoints[0].timeSeconds == Catch::Approx(0.5));
}

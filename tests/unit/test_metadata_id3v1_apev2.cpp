// M15: ID3v1/ID3v1.1 parsing, APEv2 parsing, and the cross-format conflict-reporting requirement
// ("a file with conflicting ID3v1/ID3v2 tags reports both").

#include <catch2/catch_test_macros.hpp>

#include "../../engine/metadata/apev2.hpp"
#include "../../engine/metadata/id3v1.hpp"
#include "../../engine/metadata/metadata.hpp"
#include "metadata_test_bytes.hpp"

using namespace aud::metadata;
using namespace aud::metadata::test;

namespace {

Bytes fixedField(const std::string& text, std::size_t width) {
    Bytes out;
    appendStr(out, text);
    appendNul(out, width - text.size());
    return out;
}

}  // namespace

TEST_CASE("id3v1: a well-formed v1.1 tag parses title/artist/album/year/genre/track", "[metadata][id3v1]") {
    Bytes tag;
    appendStr(tag, "TAG");
    auto title  = fixedField("Test Title", 30);
    auto artist = fixedField("Test Artist", 30);
    auto album  = fixedField("Test Album", 30);
    tag.insert(tag.end(), title.begin(), title.end());
    tag.insert(tag.end(), artist.begin(), artist.end());
    tag.insert(tag.end(), album.begin(), album.end());
    appendStr(tag, "2005");                    // year, 4 bytes
    auto comment = fixedField("hi", 28);        // v1.1: 28-byte comment
    tag.insert(tag.end(), comment.begin(), comment.end());
    appendByte(tag, 0);   // byte 125 == 0 -> v1.1 marker
    appendByte(tag, 7);   // byte 126 == track number
    appendByte(tag, 17);  // genre index 17 == "Rock" (standard ID3v1 genre list)

    REQUIRE(tag.size() == 128);

    Id3v1ParseResult result = parseId3v1(tag);
    REQUIRE(result.present);
    REQUIRE(result.tags.title.has_value());
    CHECK(*result.tags.title == "Test Title");
    REQUIRE(result.tags.artist.has_value());
    CHECK(*result.tags.artist == "Test Artist");
    REQUIRE(result.tags.album.has_value());
    CHECK(*result.tags.album == "Test Album");
    REQUIRE(result.tags.year.has_value());
    CHECK(*result.tags.year == 2005);
    REQUIRE(result.tags.trackNumber.has_value());
    CHECK(*result.tags.trackNumber == 7);
    REQUIRE(result.tags.genre.has_value());
    CHECK(*result.tags.genre == "Rock");
}

TEST_CASE("id3v1: absent (no TAG magic) reports present=false, not an error", "[metadata][id3v1]") {
    Bytes notATag(128, std::byte{0});
    Id3v1ParseResult result = parseId3v1(notATag);
    CHECK_FALSE(result.present);
}

TEST_CASE("metadata: conflicting ID3v1/ID3v2 titles are both reported, ID3v2 wins the mapped field",
          "[metadata][conflict]") {
    TagSet v2;
    v2.sourceFormat = "id3v2.3";
    v2.title         = "V2 Title";

    TagSet v1;
    v1.sourceFormat = "id3v1";
    v1.title         = "V1 Title";

    // Priority order per M15: ID3v2 before ID3v1.
    Metadata merged = mergeTagSets({v2, v1});

    REQUIRE(merged.title.has_value());
    CHECK(*merged.title == "V2 Title");

    bool sawV2Conflict = false, sawV1Conflict = false;
    for (const auto& [field, value] : merged.fieldConflicts) {
        if (field != "title") continue;
        if (value.text == "V2 Title" && value.sourceFormat == "id3v2.3") sawV2Conflict = true;
        if (value.text == "V1 Title" && value.sourceFormat == "id3v1") sawV1Conflict = true;
    }
    CHECK(sawV2Conflict);
    CHECK(sawV1Conflict);
}

TEST_CASE("apev2: a well-formed footer-only tag parses text items", "[metadata][apev2]") {
    Bytes items;
    // "Title" = "My Title"
    appendU32Le(items, 8);  // value size
    appendU32Le(items, 0);  // item flags: type 0 = UTF-8 text
    appendStr(items, "Title");
    appendByte(items, 0);
    appendStr(items, "My Title");

    Bytes file;
    file.insert(file.end(), items.begin(), items.end());

    const std::uint32_t tagSize = 32 + static_cast<std::uint32_t>(items.size());
    appendStr(file, "APETAGEX");
    appendU32Le(file, 2000);      // version
    appendU32Le(file, tagSize);   // tag size (items + footer, excludes header)
    appendU32Le(file, 1);         // item count
    appendU32Le(file, 0);         // flags: this is a footer (bit 29 clear), no header present
    appendNul(file, 8);           // reserved

    Apev2ParseResult result = parseApev2(file);
    REQUIRE(result.present);
    REQUIRE(result.tags.title.has_value());
    CHECK(*result.tags.title == "My Title");
}

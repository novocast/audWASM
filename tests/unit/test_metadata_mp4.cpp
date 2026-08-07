// M15: MP4 `ilst` parsing via the general box_reader — text atoms, trkn (binary index/total), and
// a "----" freeform atom (MusicBrainz Track Id).

#include <catch2/catch_test_macros.hpp>

#include "../../engine/metadata/mp4/box_reader.hpp"
#include "../../engine/metadata/mp4/ilst.hpp"
#include "metadata_test_bytes.hpp"

using namespace aud::metadata;
using namespace aud::metadata::test;

namespace {

Bytes box(const std::string& type, const Bytes& body) {
    Bytes out;
    appendU32Be(out, static_cast<std::uint32_t>(8 + body.size()));
    appendStr(out, type);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// A "data" atom: [type indicator 4BE][locale 4BE=0][value].
Bytes dataAtom(std::uint32_t typeIndicator, const Bytes& value) {
    Bytes body;
    appendU32Be(body, typeIndicator);
    appendU32Be(body, 0);
    body.insert(body.end(), value.begin(), value.end());
    return box("data", body);
}

Bytes textItem(const std::string& fourcc, const std::string& text) {
    Bytes value;
    appendStr(value, text);
    return box(fourcc, dataAtom(1, value));
}

}  // namespace

TEST_CASE("box_reader: iterates sibling boxes and stops cleanly at a truncated tail", "[metadata][mp4]") {
    Bytes bytes;
    Bytes a = box("aaaa", {});
    Bytes b = box("bbbb", {});
    bytes.insert(bytes.end(), a.begin(), a.end());
    bytes.insert(bytes.end(), b.begin(), b.end());
    bytes.push_back(std::byte{0xFF});  // one dangling byte — not a valid box header

    std::vector<std::string> seen;
    std::size_t                count =
        mp4::forEachBox(bytes, [&](const mp4::Box& childBox) { seen.push_back(childBox.type); });

    CHECK(count == 2);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == "aaaa");
    CHECK(seen[1] == "bbbb");
}

TEST_CASE("mp4 ilst: text atoms, trkn, and a freeform MusicBrainz atom are all mapped", "[metadata][mp4][ilst]") {
    Bytes ilstItems;
    Bytes nam = textItem("\xA9" "nam", "Test Title");
    ilstItems.insert(ilstItems.end(), nam.begin(), nam.end());
    Bytes art = textItem("\xA9" "ART", "Test Artist");
    ilstItems.insert(ilstItems.end(), art.begin(), art.end());

    Bytes trknValue;
    appendU16Be(trknValue, 0);   // reserved
    appendU16Be(trknValue, 3);   // track number
    appendU16Be(trknValue, 12);  // track total
    appendU16Be(trknValue, 0);   // reserved
    Bytes trkn = box("trkn", dataAtom(0, trknValue));
    ilstItems.insert(ilstItems.end(), trkn.begin(), trkn.end());

    Bytes meanBody;
    appendU32Be(meanBody, 0);
    appendStr(meanBody, "com.apple.iTunes");
    Bytes nameBody;
    appendU32Be(nameBody, 0);
    appendStr(nameBody, "MusicBrainz Track Id");
    Bytes freeformValue;
    appendStr(freeformValue, "abc-123-def");
    Bytes freeform;
    Bytes meanBox   = box("mean", meanBody);
    Bytes nameBox    = box("name", nameBody);
    Bytes dataBox    = dataAtom(1, freeformValue);
    freeform.insert(freeform.end(), meanBox.begin(), meanBox.end());
    freeform.insert(freeform.end(), nameBox.begin(), nameBox.end());
    freeform.insert(freeform.end(), dataBox.begin(), dataBox.end());
    Bytes freeformItem = box("----", freeform);
    ilstItems.insert(ilstItems.end(), freeformItem.begin(), freeformItem.end());

    Bytes ilst = box("ilst", ilstItems);
    Bytes meta;
    appendU32Be(meta, 0);  // version+flags
    meta.insert(meta.end(), ilst.begin(), ilst.end());
    Bytes metaBox = box("meta", meta);
    Bytes udta     = box("udta", metaBox);
    Bytes moov     = box("moov", udta);

    Bytes ftyp = box("ftyp", []{ Bytes b; appendStr(b, "M4A "); appendU32Be(b, 0); appendStr(b, "M4A "); return b; }());

    Bytes file;
    file.insert(file.end(), ftyp.begin(), ftyp.end());
    file.insert(file.end(), moov.begin(), moov.end());

    TagSet tags = mp4::parseIlst(file);
    REQUIRE(tags.title.has_value());
    CHECK(*tags.title == "Test Title");
    REQUIRE(tags.artist.has_value());
    CHECK(*tags.artist == "Test Artist");
    REQUIRE(tags.trackNumber.has_value());
    CHECK(*tags.trackNumber == 3);
    REQUIRE(tags.trackTotal.has_value());
    CHECK(*tags.trackTotal == 12);
    REQUIRE(tags.musicBrainzTrackId.has_value());
    CHECK(*tags.musicBrainzTrackId == "abc-123-def");
}

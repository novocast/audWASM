// M15: LRC detection inside plain unsynced lyric fields — "very common; detect the [mm:ss.xx]
// pattern and parse it".

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../engine/metadata/lyrics.hpp"

using namespace aud::metadata;

TEST_CASE("lyrics: plain text with no timestamps is not LRC", "[metadata][lyrics]") {
    CHECK_FALSE(looksLikeLrc("Just some plain lyrics\nwith no timing info at all"));
}

TEST_CASE("lyrics: [mm:ss.xx] tags are detected and parsed in order", "[metadata][lyrics]") {
    std::string text =
        "[ar:Some Artist]\n"
        "[00:01.00]First line\n"
        "[00:02.50]Second line\n"
        "[01:00.00]Third line\n";

    REQUIRE(looksLikeLrc(text));
    auto lines = parseLrc(text);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].timeSeconds == Catch::Approx(1.0));
    CHECK(lines[0].text == "First line");
    CHECK(lines[1].timeSeconds == Catch::Approx(2.5));
    CHECK(lines[2].timeSeconds == Catch::Approx(60.0));
    CHECK(lines[2].text == "Third line");
}

TEST_CASE("lyrics: a line with two timestamps repeats at both times", "[metadata][lyrics]") {
    std::string text = "[00:10.00][00:20.00]Chorus\n";
    auto         lines = parseLrc(text);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "Chorus");
    CHECK(lines[1].text == "Chorus");
}

TEST_CASE("lyrics: makeLyricsFromUnsyncedText detects LRC and falls back to plain text otherwise",
          "[metadata][lyrics]") {
    Lyrics synced = makeLyricsFromUnsyncedText("[00:05.00]Hello\n", "eng", "", "id3v2.4");
    CHECK(synced.synced);
    REQUIRE(synced.lines.size() == 1);
    CHECK(synced.lines[0].timeSeconds == Catch::Approx(5.0));

    Lyrics plain = makeLyricsFromUnsyncedText("Just plain lyrics.", "eng", "", "id3v2.4");
    CHECK_FALSE(plain.synced);
    REQUIRE(plain.lines.size() == 1);
    CHECK(plain.lines[0].timeSeconds < 0.0);
    CHECK(plain.lines[0].text == "Just plain lyrics.");
}

// M15: each ID3v2 text encoding, including the mislabelled-Latin-1-that-is-really-UTF-8 heuristic.

#include <catch2/catch_test_macros.hpp>

#include "../../engine/metadata/text_encoding.hpp"
#include "metadata_test_bytes.hpp"

using namespace aud::metadata;
using namespace aud::metadata::test;

TEST_CASE("text_encoding: Latin-1 decodes each byte as its own codepoint", "[metadata][text_encoding]") {
    Bytes bytes;
    appendBytes(bytes, {'c', 'a', 'f', 0xE9});  // "caf" + Latin-1 e-acute
    std::string result = decodeToUtf8(bytes, TextEncoding::Latin1);
    CHECK(result == "caf\xC3\xA9");  // UTF-8 for U+00E9
}

TEST_CASE("text_encoding: UTF-16 with a little-endian BOM", "[metadata][text_encoding]") {
    Bytes bytes;
    appendBytes(bytes, {0xFF, 0xFE});  // LE BOM
    appendBytes(bytes, {'H', 0, 'i', 0});
    std::string result = decodeToUtf8(bytes, TextEncoding::Utf16Bom);
    CHECK(result == "Hi");
}

TEST_CASE("text_encoding: UTF-16 with a big-endian BOM", "[metadata][text_encoding]") {
    Bytes bytes;
    appendBytes(bytes, {0xFE, 0xFF});  // BE BOM
    appendBytes(bytes, {0, 'H', 0, 'i'});
    std::string result = decodeToUtf8(bytes, TextEncoding::Utf16Bom);
    CHECK(result == "Hi");
}

TEST_CASE("text_encoding: UTF-16BE with no BOM (v2.4 only)", "[metadata][text_encoding]") {
    Bytes bytes;
    appendBytes(bytes, {0, 'H', 0, 'i'});
    std::string result = decodeToUtf8(bytes, TextEncoding::Utf16Be);
    CHECK(result == "Hi");
}

TEST_CASE("text_encoding: UTF-16 surrogate pair decodes to the correct codepoint", "[metadata][text_encoding]") {
    // U+1F600 (grinning face) as a UTF-16BE surrogate pair: D83D DE00.
    Bytes bytes;
    appendBytes(bytes, {0xD8, 0x3D, 0xDE, 0x00});
    std::string result = decodeToUtf8(bytes, TextEncoding::Utf16Be);
    CHECK(result == "\xF0\x9F\x98\x80");  // UTF-8 for U+1F600
}

TEST_CASE("text_encoding: declared UTF-8 passes through verbatim", "[metadata][text_encoding]") {
    Bytes bytes;
    appendStr(bytes, "日本語");  // already valid UTF-8 in the source file
    std::string result = decodeToUtf8(bytes, TextEncoding::Utf8);
    CHECK(result == "日本語");
}

TEST_CASE("text_encoding: mislabelled Latin-1 that is really UTF-8 is heuristically reinterpreted",
          "[metadata][text_encoding]") {
    // A frame that (wrongly) declares encoding=Latin-1 but actually contains valid multibyte UTF-8
    // (Cyrillic "Привет") — the naive Latin-1 decode would mangle every byte into mojibake; the
    // heuristic must detect this and reinterpret verbatim instead.
    Bytes bytes;
    appendStr(bytes, "Привет");

    bool        mislabelled = false;
    std::string result       = decodeToUtf8(bytes, TextEncoding::Latin1, &mislabelled);

    CHECK(mislabelled);
    CHECK(result == "Привет");
}

TEST_CASE("text_encoding: genuine single-byte Latin-1 text is not flagged as mislabelled",
          "[metadata][text_encoding]") {
    Bytes bytes;
    appendBytes(bytes, {'N', 0xE9, 'e'});  // "Née" in real Latin-1 (0xE9 alone isn't valid UTF-8)

    bool        mislabelled = false;
    std::string result       = decodeToUtf8(bytes, TextEncoding::Latin1, &mislabelled);

    CHECK_FALSE(mislabelled);
    CHECK(result == "N\xC3\xA9" "e");  // correctly converted to UTF-8, not passed through raw
}

TEST_CASE("text_encoding: multi-value v2.4 text frame splits on 0x00", "[metadata][text_encoding]") {
    std::string joined = std::string("Artist One") + '\0' + "Artist Two";
    auto        values  = splitNulSeparated(joined);
    REQUIRE(values.size() == 2);
    CHECK(values[0] == "Artist One");
    CHECK(values[1] == "Artist Two");
}

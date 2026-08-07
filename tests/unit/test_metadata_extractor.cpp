// M15: the top-level extract() dispatch — container sniffing, and the "no tags at all produces an
// empty-but-valid result, not an error" acceptance criterion.

#include <catch2/catch_test_macros.hpp>

#include "../../engine/metadata/metadata.hpp"
#include "metadata_test_bytes.hpp"

using namespace aud::metadata;
using namespace aud::metadata::test;

TEST_CASE("metadata: an empty buffer produces an empty-but-valid result, not an error", "[metadata][extract]") {
    Bytes empty;
    aud::Result<Metadata> result = extract(empty);
    REQUIRE(result.has_value());
    CHECK_FALSE(result.value().title.has_value());
    CHECK(result.value().pictures.empty());
}

TEST_CASE("metadata: random/garbage bytes produce an empty-but-valid result", "[metadata][extract]") {
    Bytes garbage;
    for (int i = 0; i < 256; ++i) appendByte(garbage, static_cast<std::uint8_t>((i * 137 + 7) & 0xFF));

    aud::Result<Metadata> result = extract(garbage);
    REQUIRE(result.has_value());
    // No specific expectations about content — the acceptance criterion is just "doesn't crash and
    // isn't reported as an error", which REQUIRE(has_value()) above already establishes.
}

TEST_CASE("metadata: a FLAC file is dispatched to the Vorbis-comment parser", "[metadata][extract]") {
    Bytes streamInfoBody(34, std::byte{0});
    Bytes commentPayload;
    appendU32Le(commentPayload, 0);  // empty vendor string
    appendU32Le(commentPayload, 1);  // 1 comment
    std::string comment = "TITLE=Dispatched Title";
    appendU32Le(commentPayload, static_cast<std::uint32_t>(comment.size()));
    appendStr(commentPayload, comment);

    auto block = [](std::uint8_t type, bool isLast, const Bytes& body) {
        Bytes out;
        appendByte(out, static_cast<std::uint8_t>((isLast ? 0x80 : 0) | (type & 0x7F)));
        appendU24Be(out, static_cast<std::uint32_t>(body.size()));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    };

    Bytes file;
    appendStr(file, "fLaC");
    Bytes info    = block(0, false, streamInfoBody);
    Bytes comments = block(4, true, commentPayload);
    file.insert(file.end(), info.begin(), info.end());
    file.insert(file.end(), comments.begin(), comments.end());

    aud::Result<Metadata> result = extract(file);
    REQUIRE(result.has_value());
    REQUIRE(result.value().title.has_value());
    CHECK(*result.value().title == "Dispatched Title");
}

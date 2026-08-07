#include "ilst.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "../lyrics.hpp"
#include "../pictures.hpp"
#include "../replaygain.hpp"
#include "box_reader.hpp"

namespace aud::metadata::mp4 {

namespace {

std::uint32_t readU32Be(std::span<const std::byte> b, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(b[offset]) << 24) | (static_cast<std::uint32_t>(b[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(b[offset + 2]) << 8) | static_cast<std::uint32_t>(b[offset + 3]);
}

// Finds the first direct child box of `bytes` named `type`, if any.
std::optional<Box> findChild(std::span<const std::byte> bytes, const std::string& type) {
    std::optional<Box> found;
    forEachBox(bytes, [&](const Box& box) {
        if (!found.has_value() && box.type == type) found = box;
    });
    return found;
}

struct DataAtomValue {
    std::uint32_t                type = 0;  // well-known type indicator: 1=UTF-8, 13=JPEG, 14=PNG, 21=int, ...
    std::span<const std::byte>   value;
};

std::optional<DataAtomValue> readDataAtom(const Box& box) {
    if (box.type != "data" || box.body.size() < 8) return std::nullopt;
    DataAtomValue out;
    out.type  = readU32Be(box.body, 0);
    out.value = box.body.subspan(8);
    return out;
}

std::string dataAtomText(const Box& itemBox) {
    auto data = findChild(itemBox.body, "data");
    if (!data) return {};
    auto value = readDataAtom(*data);
    if (!value) return {};
    return std::string(reinterpret_cast<const char*>(value->value.data()), value->value.size());
}

std::uint64_t bigEndianUint(std::span<const std::byte> bytes) noexcept {
    std::uint64_t v = 0;
    for (std::byte b : bytes) v = (v << 8) | static_cast<std::uint8_t>(b);
    return v;
}

// trkn/disk: data atom value is typically 8 bytes: 2 reserved, 2 BE number, 2 BE total, 2 reserved.
// Some encoders emit only 6 bytes (no trailing reserved); both are handled.
void parseIndexTotalAtom(const Box& itemBox, std::optional<std::uint32_t>& number, std::optional<std::uint32_t>& total) {
    auto data = findChild(itemBox.body, "data");
    if (!data) return;
    auto value = readDataAtom(*data);
    if (!value || value->value.size() < 6) return;
    number = static_cast<std::uint32_t>(bigEndianUint(value->value.subspan(2, 2)));
    if (value->value.size() >= 6) total = static_cast<std::uint32_t>(bigEndianUint(value->value.subspan(4, 2)));
}

void parseFreeformAtom(const Box& itemBox, const std::string& sourceFormat, TagSet& tags) {
    // "----" atoms carry three children: `mean` (reverse-DNS namespace), `name` (the free-form key),
    // `data` (the value) — the namespace itself is the same for every iTunes-authored freeform tag
    // ("com.apple.iTunes") so only `name` is used to route.
    auto meanBox = findChild(itemBox.body, "mean");
    auto nameBox = findChild(itemBox.body, "name");
    auto dataBox = findChild(itemBox.body, "data");
    if (!nameBox || !dataBox) return;
    (void)meanBox;

    // `mean`/`name` bodies are [4-byte version+flags][text] — the same convention as `data`.
    const std::string name = nameBox->body.size() > 4
                                  ? std::string(reinterpret_cast<const char*>(nameBox->body.data() + 4),
                                                nameBox->body.size() - 4)
                                  : std::string();
    auto value = readDataAtom(*dataBox);
    if (!value) return;
    const std::string text(reinterpret_cast<const char*>(value->value.data()), value->value.size());

    if (name == "MusicBrainz Track Id") {
        if (!tags.musicBrainzTrackId) tags.musicBrainzTrackId = text;
    } else if (name == "MusicBrainz Album Id") {
        if (!tags.musicBrainzAlbumId) tags.musicBrainzAlbumId = text;
    } else if (name == "CATALOGNUMBER") {
        if (!tags.catalogNumber) tags.catalogNumber = text;
    } else if (name == "UPC" || name == "BARCODE") {
        if (!tags.upc) tags.upc = text;
    } else if (name == "iTunNORM") {
        if (auto gain = parseItunNormTrackGainDb(text)) {
            ReplayGainSource src;
            src.origin      = "mp4.itunnorm";
            src.trackGainDb = gain;
            tags.replayGainSources.push_back(src);
        }
        tags.unmapped.emplace_back("iTunNORM", MetadataValue{text, sourceFormat, "----:com.apple.iTunes:iTunNORM"});
    } else if (!name.empty()) {
        tags.unmapped.emplace_back(name, MetadataValue{text, sourceFormat, "----:" + name});
    }
}

void parseCovr(const Box& itemBox, const std::string& sourceFormat, TagSet& tags) {
    forEachBox(itemBox.body, [&](const Box& child) {
        auto value = readDataAtom(child);
        if (!value) return;
        std::string mime;
        if (value->type == 13) mime = "image/jpeg";
        else if (value->type == 14) mime = "image/png";

        Picture pic;
        if (extractPicture(value->value, mime, PictureType::FrontCover, "", sourceFormat, pic, tags.diagnostics)) {
            tags.pictures.push_back(std::move(pic));
        }
    });
}

}  // namespace

TagSet parseIlst(std::span<const std::byte> fileBytes) {
    TagSet tags;
    tags.sourceFormat = "mp4";

    auto moov = findChild(fileBytes, "moov");
    if (!moov) return tags;
    auto udta = findChild(moov->body, "udta");
    if (!udta) return tags;

    std::optional<Box> ilstBox = findChild(udta->body, "ilst");
    if (!ilstBox) {
        if (auto meta = findChild(udta->body, "meta")) {
            // `meta` is a "full box": 4-byte version+flags before its children.
            std::span<const std::byte> metaBody =
                meta->body.size() >= 4 ? meta->body.subspan(4) : std::span<const std::byte>();
            ilstBox = findChild(metaBody, "ilst");
        }
    }
    if (!ilstBox) return tags;

    forEachBox(ilstBox->body, [&](const Box& item) {
        // Adjacent-string-literal concatenation (e.g. "\xA9" "nam") is required, not decorative:
        // a bare "\xA9nam" would let the hex escape greedily consume any following hex digits
        // ('a'/'A'..'f'/'F') as part of the escape — "\xA9ART" is `\xA9A` + "RT", which overflows a
        // char and fails to compile. Splitting the literal caps the escape at exactly one byte.
        if (item.type == "\xA9" "nam") {
            if (!tags.title) tags.title = dataAtomText(item);
        } else if (item.type == "\xA9" "ART") {
            if (!tags.artist) tags.artist = dataAtomText(item);
        } else if (item.type == "aART") {
            if (!tags.albumArtist) tags.albumArtist = dataAtomText(item);
        } else if (item.type == "\xA9" "alb") {
            if (!tags.album) tags.album = dataAtomText(item);
        } else if (item.type == "\xA9" "gen") {
            if (!tags.genre) tags.genre = dataAtomText(item);
        } else if (item.type == "\xA9" "wrt") {
            if (!tags.composer) tags.composer = dataAtomText(item);
        } else if (item.type == "\xA9" "cmt") {
            if (!tags.comment) tags.comment = dataAtomText(item);
        } else if (item.type == "\xA9" "too") {
            if (!tags.encoderSettings) tags.encoderSettings = dataAtomText(item);
        } else if (item.type == "cprt") {
            if (!tags.copyright) tags.copyright = dataAtomText(item);
        } else if (item.type == "\xA9" "lyr") {
            std::string text = dataAtomText(item);
            if (!text.empty()) tags.lyrics.push_back(makeLyricsFromUnsyncedText(text, "", "", "mp4"));
        } else if (item.type == "\xA9" "day") {
            std::string text = dataAtomText(item);
            if (!text.empty()) {
                if (!tags.date) tags.date = text;
                if (!tags.year && text.size() >= 4) {
                    bool allDigits = std::all_of(text.begin(), text.begin() + 4,
                                                  [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
                    if (allDigits) tags.year = static_cast<std::uint32_t>(std::strtoul(text.substr(0, 4).c_str(), nullptr, 10));
                }
            }
        } else if (item.type == "trkn") {
            parseIndexTotalAtom(item, tags.trackNumber, tags.trackTotal);
        } else if (item.type == "disk") {
            parseIndexTotalAtom(item, tags.discNumber, tags.discTotal);
        } else if (item.type == "tmpo") {
            auto data = findChild(item.body, "data");
            if (data) {
                auto value = readDataAtom(*data);
                if (value && !value->value.empty() && !tags.bpm) {
                    tags.bpm = static_cast<std::uint32_t>(bigEndianUint(value->value));
                }
            }
        } else if (item.type == "covr") {
            parseCovr(item, "mp4", tags);
        } else if (item.type == "----") {
            parseFreeformAtom(item, "mp4", tags);
        } else if (item.type != "free" && item.type != "\xA9" "too") {
            std::string text = dataAtomText(item);
            if (!text.empty()) tags.unmapped.emplace_back(item.type, MetadataValue{text, "mp4", item.type});
        }
    });

    return tags;
}

}  // namespace aud::metadata::mp4

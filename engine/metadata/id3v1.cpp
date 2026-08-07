#include "id3v1.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "text_encoding.hpp"

namespace aud::metadata {

namespace {

constexpr std::size_t kTagSize = 128;

// The classic ID3v1 genre list plus the Winamp extension, index-addressed. Anything beyond this
// table (or 0xFF, ID3v1's own "no genre" sentinel) is left unmapped rather than guessed.
constexpr std::array<const char*, 148> kGenres = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz",
    "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno",
    "Industrial", "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno",
    "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
    "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "AlternRock", "Bass", "Soul", "Punk",
    "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
    "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy",
    "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American",
    "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
    "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk",
    "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic",
    "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock",
    "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour",
    "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus",
    "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad",
    "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A Cappella",
    "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror", "Indie",
    "BritPop", "Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal",
    "Black Metal", "Crossover", "Contemporary Christian", "Christian Rock", "Merengue", "Salsa",
    "Thrash Metal", "Anime", "JPop", "Synthpop",
};

std::string trimField(std::span<const std::byte> field) {
    // ID3v1 text is Latin-1 (or, endemically, mislabelled UTF-8), NUL- or space-padded.
    std::size_t len = field.size();
    while (len > 0) {
        const auto c = static_cast<std::uint8_t>(field[len - 1]);
        if (c != 0 && c != ' ') break;
        --len;
    }
    return decodeToUtf8(field.subspan(0, len), TextEncoding::Latin1);
}

}  // namespace

Id3v1ParseResult parseId3v1(std::span<const std::byte> bytes) {
    Id3v1ParseResult result;
    if (bytes.size() < kTagSize) return result;

    const auto tag = bytes.subspan(bytes.size() - kTagSize, kTagSize);
    if (static_cast<char>(tag[0]) != 'T' || static_cast<char>(tag[1]) != 'A' || static_cast<char>(tag[2]) != 'G') {
        return result;
    }

    result.present            = true;
    result.tags.sourceFormat = "id3v1";

    const std::string title   = trimField(tag.subspan(3, 30));
    const std::string artist  = trimField(tag.subspan(33, 30));
    const std::string album   = trimField(tag.subspan(63, 30));
    const std::string year    = trimField(tag.subspan(93, 4));

    if (!title.empty()) result.tags.title = title;
    if (!artist.empty()) result.tags.artist = artist;
    if (!album.empty()) result.tags.album = album;
    if (!year.empty() && std::all_of(year.begin(), year.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        result.tags.year = static_cast<std::uint32_t>(std::strtoul(year.c_str(), nullptr, 10));
    }

    // ID3v1.1 convention: comment is 28 bytes (97..124), byte 125 is 0, byte 126 is the track
    // number. A genuine ID3v1 comment fills all 30 bytes (97..126) and byte 125 is very unlikely to
    // be exactly 0 by chance while still having real text follow — this is the standard heuristic.
    const bool isV11 = static_cast<std::uint8_t>(tag[125]) == 0 && static_cast<std::uint8_t>(tag[126]) != 0;
    const std::string comment = trimField(tag.subspan(97, isV11 ? 28 : 30));
    if (!comment.empty()) result.tags.comment = comment;
    if (isV11) result.tags.trackNumber = static_cast<std::uint8_t>(tag[126]);

    const auto genreIndex = static_cast<std::uint8_t>(tag[127]);
    if (genreIndex < kGenres.size()) {
        result.tags.genre = kGenres[genreIndex];
    }

    return result;
}

}  // namespace aud::metadata

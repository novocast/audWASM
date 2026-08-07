#include "tag_set.hpp"

namespace aud::metadata {

namespace {

std::string toText(const std::string& v) { return v; }
std::string toText(std::uint32_t v) { return std::to_string(v); }

// Merges one optional<T> field across all `sets` (already priority-ordered): the first non-empty
// value wins into `out`; every subsequent *different* non-empty value is recorded as a conflict
// (and, the first time a conflict is seen for this field, the winner is recorded too — so
// `fieldConflicts` always shows the full disagreement, not just the losers).
template <class T>
void mergeOptionalField(const std::vector<TagSet>& sets, const char* fieldName, std::optional<T> TagSet::*member,
                         std::optional<T>&                                    out,
                         std::vector<std::pair<std::string, MetadataValue>>& conflicts) {
    bool        hasWinner      = false;
    T           winnerValue{};
    std::string winnerFormat;
    bool        conflictLogged = false;

    for (const auto& set : sets) {
        const std::optional<T>& value = set.*member;
        if (!value.has_value()) continue;

        if (!hasWinner) {
            hasWinner    = true;
            winnerValue  = *value;
            winnerFormat = set.sourceFormat;
            out          = winnerValue;
            continue;
        }

        if (!(*value == winnerValue)) {
            if (!conflictLogged) {
                conflicts.emplace_back(fieldName, MetadataValue{toText(winnerValue), winnerFormat, fieldName});
                conflictLogged = true;
            }
            conflicts.emplace_back(fieldName, MetadataValue{toText(*value), set.sourceFormat, fieldName});
        }
    }
}

}  // namespace

Metadata mergeTagSets(const std::vector<TagSet>& sets) {
    Metadata out;

    mergeOptionalField(sets, "title", &TagSet::title, out.title, out.fieldConflicts);
    mergeOptionalField(sets, "artist", &TagSet::artist, out.artist, out.fieldConflicts);
    mergeOptionalField(sets, "albumArtist", &TagSet::albumArtist, out.albumArtist, out.fieldConflicts);
    mergeOptionalField(sets, "album", &TagSet::album, out.album, out.fieldConflicts);
    mergeOptionalField(sets, "genre", &TagSet::genre, out.genre, out.fieldConflicts);
    mergeOptionalField(sets, "composer", &TagSet::composer, out.composer, out.fieldConflicts);
    mergeOptionalField(sets, "comment", &TagSet::comment, out.comment, out.fieldConflicts);
    mergeOptionalField(sets, "publisher", &TagSet::publisher, out.publisher, out.fieldConflicts);
    mergeOptionalField(sets, "copyright", &TagSet::copyright, out.copyright, out.fieldConflicts);
    mergeOptionalField(sets, "encodedBy", &TagSet::encodedBy, out.encodedBy, out.fieldConflicts);
    mergeOptionalField(sets, "encoderSettings", &TagSet::encoderSettings, out.encoderSettings, out.fieldConflicts);

    mergeOptionalField(sets, "year", &TagSet::year, out.year, out.fieldConflicts);
    mergeOptionalField(sets, "trackNumber", &TagSet::trackNumber, out.trackNumber, out.fieldConflicts);
    mergeOptionalField(sets, "trackTotal", &TagSet::trackTotal, out.trackTotal, out.fieldConflicts);
    mergeOptionalField(sets, "discNumber", &TagSet::discNumber, out.discNumber, out.fieldConflicts);
    mergeOptionalField(sets, "discTotal", &TagSet::discTotal, out.discTotal, out.fieldConflicts);
    mergeOptionalField(sets, "bpm", &TagSet::bpm, out.bpm, out.fieldConflicts);

    mergeOptionalField(sets, "isrc", &TagSet::isrc, out.isrc, out.fieldConflicts);
    mergeOptionalField(sets, "upc", &TagSet::upc, out.upc, out.fieldConflicts);
    mergeOptionalField(sets, "catalogNumber", &TagSet::catalogNumber, out.catalogNumber, out.fieldConflicts);
    mergeOptionalField(sets, "musicBrainzTrackId", &TagSet::musicBrainzTrackId, out.musicBrainzTrackId,
                        out.fieldConflicts);
    mergeOptionalField(sets, "musicBrainzAlbumId", &TagSet::musicBrainzAlbumId, out.musicBrainzAlbumId,
                        out.fieldConflicts);
    mergeOptionalField(sets, "date", &TagSet::date, out.date, out.fieldConflicts);

    for (const auto& set : sets) {
        for (const auto& src : set.replayGainSources) out.replayGain.sources.push_back(src);
        for (const auto& pic : set.pictures) out.pictures.push_back(pic);
        for (const auto& lyr : set.lyrics) out.lyrics.push_back(lyr);
        for (const auto& cue : set.cuePoints) out.cuePoints.push_back(cue);
        if (set.broadcast.present && !out.broadcast.present) out.broadcast = set.broadcast;
        for (const auto& u : set.unmapped) out.unmapped.push_back(u);
        for (const auto& d : set.diagnostics) out.diagnostics.push_back(d);
    }

    return out;
}

namespace {
template <class T>
void takeIfEmpty(std::optional<T>& dst, std::optional<T>&& src) {
    if (!dst.has_value() && src.has_value()) dst = std::move(src);
}
}  // namespace

void appendTagSet(TagSet& dst, TagSet src) {
    takeIfEmpty(dst.title, std::move(src.title));
    takeIfEmpty(dst.artist, std::move(src.artist));
    takeIfEmpty(dst.albumArtist, std::move(src.albumArtist));
    takeIfEmpty(dst.album, std::move(src.album));
    takeIfEmpty(dst.genre, std::move(src.genre));
    takeIfEmpty(dst.composer, std::move(src.composer));
    takeIfEmpty(dst.comment, std::move(src.comment));
    takeIfEmpty(dst.publisher, std::move(src.publisher));
    takeIfEmpty(dst.copyright, std::move(src.copyright));
    takeIfEmpty(dst.encodedBy, std::move(src.encodedBy));
    takeIfEmpty(dst.encoderSettings, std::move(src.encoderSettings));
    takeIfEmpty(dst.year, std::move(src.year));
    takeIfEmpty(dst.trackNumber, std::move(src.trackNumber));
    takeIfEmpty(dst.trackTotal, std::move(src.trackTotal));
    takeIfEmpty(dst.discNumber, std::move(src.discNumber));
    takeIfEmpty(dst.discTotal, std::move(src.discTotal));
    takeIfEmpty(dst.bpm, std::move(src.bpm));
    takeIfEmpty(dst.isrc, std::move(src.isrc));
    takeIfEmpty(dst.upc, std::move(src.upc));
    takeIfEmpty(dst.catalogNumber, std::move(src.catalogNumber));
    takeIfEmpty(dst.musicBrainzTrackId, std::move(src.musicBrainzTrackId));
    takeIfEmpty(dst.musicBrainzAlbumId, std::move(src.musicBrainzAlbumId));
    takeIfEmpty(dst.date, std::move(src.date));

    if (src.broadcast.present && !dst.broadcast.present) dst.broadcast = std::move(src.broadcast);

    for (auto& v : src.replayGainSources) dst.replayGainSources.push_back(std::move(v));
    for (auto& v : src.pictures) dst.pictures.push_back(std::move(v));
    for (auto& v : src.lyrics) dst.lyrics.push_back(std::move(v));
    for (auto& v : src.cuePoints) dst.cuePoints.push_back(std::move(v));
    for (auto& v : src.unmapped) dst.unmapped.push_back(std::move(v));
    for (auto& v : src.diagnostics) dst.diagnostics.push_back(std::move(v));
}

}  // namespace aud::metadata

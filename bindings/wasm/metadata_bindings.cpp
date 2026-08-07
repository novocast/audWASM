// Embind surface for M15's metadata extractor. Unlike Loudness/Dc/Statistics/etc. (streaming
// Analyzers driven chunk-by-chunk against an already-decoded AudioBuffer), metadata reads the raw
// *encoded* file bytes directly and is a single one-shot call — closer in shape to
// DecodeSessionHandle::create() than to the analyser handles' processAvailableChunks()/finish()
// polling contract.
//
// Picture bytes can be arbitrarily large (cover art routinely runs into the megabytes), so they're
// never packed into the result val (M01's rule: bulk data goes back as a heap {ptr,count} view,
// never through Embind's val machinery) — the handle keeps the parsed aud::metadata::Metadata
// alive for as long as JS needs it, and pictureDataPtr()/pictureDataCount() hand back a view into
// that still-owned std::vector<std::byte> per picture. The TS wrapper must copy those bytes out
// (heap_view.ts's uint8View(...).slice()) before the handle is deleted.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "../../engine/metadata/metadata.hpp"

using emscripten::val;

namespace bindings {

namespace {

val stringOrNull(const std::optional<std::string>& v) { return v ? val(*v) : val::null(); }
val uintOrNull(const std::optional<std::uint32_t>& v) { return v ? val(*v) : val::null(); }
val doubleOrNull(const std::optional<double>& v) { return v ? val(*v) : val::null(); }

val replayGainToVal(const aud::metadata::ReplayGainInfo& gain) {
    val sources = val::array();
    for (std::size_t i = 0; i < gain.sources.size(); ++i) {
        const auto& s = gain.sources[i];
        val         out = val::object();
        out.set("origin", s.origin);
        out.set("trackGainDb", doubleOrNull(s.trackGainDb));
        out.set("trackPeak", doubleOrNull(s.trackPeak));
        out.set("albumGainDb", doubleOrNull(s.albumGainDb));
        out.set("albumPeak", doubleOrNull(s.albumPeak));
        sources.set(i, out);
    }
    val out = val::object();
    out.set("sources", sources);
    return out;
}

// Metadata *about* each picture — the pixel bytes themselves are fetched separately via
// pictureDataPtr(index)/pictureDataCount(index), keyed by the same index used here.
val picturesToVal(const std::vector<aud::metadata::Picture>& pictures) {
    val out = val::array();
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        const auto& p    = pictures[i];
        val         item = val::object();
        item.set("index", static_cast<std::uint32_t>(i));
        item.set("declaredMimeType", p.declaredMimeType);
        item.set("detectedMimeType", p.detectedMimeType);
        item.set("mimeMismatch", p.mimeMismatch);
        item.set("type", static_cast<std::uint32_t>(p.type));
        item.set("description", p.description);
        item.set("sourceFormat", p.sourceFormat);
        item.set("byteCount", static_cast<std::uint32_t>(p.data.size()));
        out.set(i, item);
    }
    return out;
}

val lyricsToVal(const std::vector<aud::metadata::Lyrics>& lyrics) {
    val out = val::array();
    for (std::size_t i = 0; i < lyrics.size(); ++i) {
        const auto& l    = lyrics[i];
        val         item = val::object();
        item.set("synced", l.synced);
        item.set("language", l.language);
        item.set("description", l.description);
        item.set("sourceFormat", l.sourceFormat);

        val lines = val::array();
        for (std::size_t j = 0; j < l.lines.size(); ++j) {
            val line = val::object();
            line.set("timeSeconds", l.lines[j].timeSeconds);
            line.set("text", l.lines[j].text);
            lines.set(j, line);
        }
        item.set("lines", lines);
        out.set(i, item);
    }
    return out;
}

val cuePointsToVal(const std::vector<aud::metadata::CuePoint>& cues) {
    val out = val::array();
    for (std::size_t i = 0; i < cues.size(); ++i) {
        val item = val::object();
        item.set("timeSeconds", cues[i].timeSeconds);
        item.set("label", cues[i].label);
        item.set("sourceFormat", cues[i].sourceFormat);
        out.set(i, item);
    }
    return out;
}

val broadcastToVal(const aud::metadata::BroadcastInfo& b) {
    val out = val::object();
    out.set("present", b.present);
    out.set("description", b.description);
    out.set("originator", b.originator);
    out.set("originatorReference", b.originatorReference);
    out.set("originationDate", b.originationDate);
    out.set("originationTime", b.originationTime);
    out.set("timeReference", static_cast<double>(b.timeReference));
    out.set("version", b.version);
    out.set("umid", b.umid);
    out.set("codingHistory", b.codingHistory);
    out.set("loudnessValueLufs", doubleOrNull(b.loudnessValueLufs));
    out.set("loudnessRangeLu", doubleOrNull(b.loudnessRangeLu));
    out.set("maxTruePeakDbtp", doubleOrNull(b.maxTruePeakDbtp));
    out.set("maxMomentaryLufs", doubleOrNull(b.maxMomentaryLufs));
    out.set("maxShortTermLufs", doubleOrNull(b.maxShortTermLufs));
    return out;
}

val metadataValueEntriesToVal(const std::vector<std::pair<std::string, aud::metadata::MetadataValue>>& entries,
                               bool includeRawKey) {
    val out = val::array();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& [key, value] = entries[i];
        val item                  = val::object();
        item.set("key", key);
        item.set("text", value.text);
        item.set("sourceFormat", value.sourceFormat);
        if (includeRawKey) item.set("rawKey", value.rawKey);
        out.set(i, item);
    }
    return out;
}

val diagnosticsToVal(const std::vector<aud::metadata::Diagnostic>& diagnostics) {
    val out = val::array();
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        val item = val::object();
        item.set("severity", static_cast<std::uint32_t>(diagnostics[i].severity));
        item.set("message", diagnostics[i].message);
        out.set(i, item);
    }
    return out;
}

}  // namespace

class MetadataHandle {
public:
    // `ptr` must point at `length` bytes already copied into the WASM heap (JS's job, via
    // HEAPU8.set() — see M01's bulk-transfer rule) holding the *entire* file, not just a probe
    // slice: unlike the decoder's sniff(), tag parsing can need to see arbitrarily far into the
    // file (a trailing ID3v1 tag, an oversized APIC block, MP4 atoms after `mdat`).
    static std::unique_ptr<MetadataHandle> create(std::uintptr_t ptr, std::size_t length) {
        const auto* bytes  = reinterpret_cast<const std::byte*>(ptr);
        auto         result = aud::metadata::extract(std::span<const std::byte>(bytes, length));
        if (!result.has_value()) {
            return nullptr;
        }
        return std::unique_ptr<MetadataHandle>(new MetadataHandle(std::move(result).value()));
    }

    val result() const {
        const auto& m = m_metadata;
        val         out = val::object();
        out.set("ok", true);

        out.set("title", stringOrNull(m.title));
        out.set("artist", stringOrNull(m.artist));
        out.set("albumArtist", stringOrNull(m.albumArtist));
        out.set("album", stringOrNull(m.album));
        out.set("genre", stringOrNull(m.genre));
        out.set("composer", stringOrNull(m.composer));
        out.set("comment", stringOrNull(m.comment));
        out.set("publisher", stringOrNull(m.publisher));
        out.set("copyright", stringOrNull(m.copyright));
        out.set("encodedBy", stringOrNull(m.encodedBy));
        out.set("encoderSettings", stringOrNull(m.encoderSettings));

        out.set("year", uintOrNull(m.year));
        out.set("trackNumber", uintOrNull(m.trackNumber));
        out.set("trackTotal", uintOrNull(m.trackTotal));
        out.set("discNumber", uintOrNull(m.discNumber));
        out.set("discTotal", uintOrNull(m.discTotal));
        out.set("bpm", uintOrNull(m.bpm));

        out.set("isrc", stringOrNull(m.isrc));
        out.set("upc", stringOrNull(m.upc));
        out.set("catalogNumber", stringOrNull(m.catalogNumber));
        out.set("musicBrainzTrackId", stringOrNull(m.musicBrainzTrackId));
        out.set("musicBrainzAlbumId", stringOrNull(m.musicBrainzAlbumId));
        out.set("date", stringOrNull(m.date));

        out.set("replayGain", replayGainToVal(m.replayGain));
        out.set("pictures", picturesToVal(m.pictures));
        out.set("lyrics", lyricsToVal(m.lyrics));
        out.set("cuePoints", cuePointsToVal(m.cuePoints));
        out.set("broadcast", broadcastToVal(m.broadcast));
        out.set("unmapped", metadataValueEntriesToVal(m.unmapped, /*includeRawKey=*/true));
        out.set("fieldConflicts", metadataValueEntriesToVal(m.fieldConflicts, /*includeRawKey=*/false));
        out.set("diagnostics", diagnosticsToVal(m.diagnostics));

        return out;
    }

    // Zero-copy view into picture `index`'s still-owned bytes — valid only until this handle is
    // deleted (or another allocating engine call runs; see heap_view.ts's growth-epoch guard). The
    // TS wrapper must materialise a copy (uint8View(...).slice()) before handing it to the caller.
    double pictureDataPtr(std::uint32_t index) const {
        if (index >= m_metadata.pictures.size()) return 0.0;
        return static_cast<double>(reinterpret_cast<std::uintptr_t>(m_metadata.pictures[index].data.data()));
    }

    std::uint32_t pictureDataCount(std::uint32_t index) const {
        if (index >= m_metadata.pictures.size()) return 0;
        return static_cast<std::uint32_t>(m_metadata.pictures[index].data.size());
    }

    std::string reportJson() const { return m_metadata.toJson(); }

private:
    explicit MetadataHandle(aud::metadata::Metadata metadata) : m_metadata(std::move(metadata)) {}

    aud::metadata::Metadata m_metadata;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_metadata) {
    emscripten::class_<bindings::MetadataHandle>("Metadata")
        .class_function("create", &bindings::MetadataHandle::create)
        .function("result", &bindings::MetadataHandle::result)
        .function("pictureDataPtr", &bindings::MetadataHandle::pictureDataPtr)
        .function("pictureDataCount", &bindings::MetadataHandle::pictureDataCount)
        .function("reportJson", &bindings::MetadataHandle::reportJson);
}

#include "metadata.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace aud::metadata {

namespace {

std::string jsonNumber(double v) {
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return v > 0 ? "1e999" : "-1e999";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

// Minimal, dependency-free JSON string escaping — tag text is attacker-controlled and may contain
// anything, including raw control characters.
std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

void writeOptionalString(std::ostringstream& out, const char* key, const std::optional<std::string>& v) {
    out << "\"" << key << "\":" << (v ? jsonString(*v) : "null");
}

void writeOptionalUint(std::ostringstream& out, const char* key, const std::optional<std::uint32_t>& v) {
    out << "\"" << key << "\":" << (v ? std::to_string(*v) : "null");
}

std::string pictureTypeName(PictureType t) {
    switch (t) {
        case PictureType::Other:              return "other";
        case PictureType::FileIcon:            return "fileIcon";
        case PictureType::OtherFileIcon:       return "otherFileIcon";
        case PictureType::FrontCover:          return "frontCover";
        case PictureType::BackCover:           return "backCover";
        case PictureType::LeafletPage:         return "leafletPage";
        case PictureType::Media:               return "media";
        case PictureType::LeadArtist:          return "leadArtist";
        case PictureType::Artist:              return "artist";
        case PictureType::Conductor:           return "conductor";
        case PictureType::Band:                return "band";
        case PictureType::Composer:            return "composer";
        case PictureType::Lyricist:            return "lyricist";
        case PictureType::RecordingLocation:   return "recordingLocation";
        case PictureType::DuringRecording:     return "duringRecording";
        case PictureType::DuringPerformance:   return "duringPerformance";
        case PictureType::VideoScreenCapture:  return "videoScreenCapture";
        case PictureType::Fish:                return "fish";
        case PictureType::Illustration:        return "illustration";
        case PictureType::BandLogo:            return "bandLogo";
        case PictureType::PublisherLogo:       return "publisherLogo";
    }
    return "other";
}

std::string severityName(Severity s) {
    switch (s) {
        case Severity::Info:    return "info";
        case Severity::Warning: return "warning";
        case Severity::Error:   return "error";
    }
    return "info";
}

}  // namespace

std::string Metadata::toJson() const {
    std::ostringstream out;
    out << "{\"schemaVersion\":\"1.0.0\",";

    writeOptionalString(out, "title", title); out << ",";
    writeOptionalString(out, "artist", artist); out << ",";
    writeOptionalString(out, "albumArtist", albumArtist); out << ",";
    writeOptionalString(out, "album", album); out << ",";
    writeOptionalString(out, "genre", genre); out << ",";
    writeOptionalString(out, "composer", composer); out << ",";
    writeOptionalString(out, "comment", comment); out << ",";
    writeOptionalString(out, "publisher", publisher); out << ",";
    writeOptionalString(out, "copyright", copyright); out << ",";
    writeOptionalString(out, "encodedBy", encodedBy); out << ",";
    writeOptionalString(out, "encoderSettings", encoderSettings); out << ",";
    writeOptionalUint(out, "year", year); out << ",";
    writeOptionalUint(out, "trackNumber", trackNumber); out << ",";
    writeOptionalUint(out, "trackTotal", trackTotal); out << ",";
    writeOptionalUint(out, "discNumber", discNumber); out << ",";
    writeOptionalUint(out, "discTotal", discTotal); out << ",";
    writeOptionalUint(out, "bpm", bpm); out << ",";
    writeOptionalString(out, "isrc", isrc); out << ",";
    writeOptionalString(out, "upc", upc); out << ",";
    writeOptionalString(out, "catalogNumber", catalogNumber); out << ",";
    writeOptionalString(out, "musicBrainzTrackId", musicBrainzTrackId); out << ",";
    writeOptionalString(out, "musicBrainzAlbumId", musicBrainzAlbumId); out << ",";
    writeOptionalString(out, "date", date); out << ",";

    out << "\"replayGain\":{\"sources\":[";
    for (std::size_t i = 0; i < replayGain.sources.size(); ++i) {
        if (i > 0) out << ",";
        const auto& s = replayGain.sources[i];
        out << "{\"origin\":" << jsonString(s.origin) << ",";
        out << "\"trackGainDb\":" << (s.trackGainDb ? jsonNumber(*s.trackGainDb) : "null") << ",";
        out << "\"trackPeak\":" << (s.trackPeak ? jsonNumber(*s.trackPeak) : "null") << ",";
        out << "\"albumGainDb\":" << (s.albumGainDb ? jsonNumber(*s.albumGainDb) : "null") << ",";
        out << "\"albumPeak\":" << (s.albumPeak ? jsonNumber(*s.albumPeak) : "null");
        out << "}";
    }
    out << "]},";

    out << "\"pictures\":[";
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        if (i > 0) out << ",";
        const auto& p = pictures[i];
        out << "{\"byteCount\":" << p.data.size() << ",";
        out << "\"declaredMimeType\":" << jsonString(p.declaredMimeType) << ",";
        out << "\"detectedMimeType\":" << jsonString(p.detectedMimeType) << ",";
        out << "\"mimeMismatch\":" << (p.mimeMismatch ? "true" : "false") << ",";
        out << "\"type\":" << jsonString(pictureTypeName(p.type)) << ",";
        out << "\"description\":" << jsonString(p.description) << ",";
        out << "\"sourceFormat\":" << jsonString(p.sourceFormat);
        out << "}";
    }
    out << "],";

    out << "\"lyrics\":[";
    for (std::size_t i = 0; i < lyrics.size(); ++i) {
        if (i > 0) out << ",";
        const auto& l = lyrics[i];
        out << "{\"synced\":" << (l.synced ? "true" : "false") << ",";
        out << "\"language\":" << jsonString(l.language) << ",";
        out << "\"description\":" << jsonString(l.description) << ",";
        out << "\"sourceFormat\":" << jsonString(l.sourceFormat) << ",";
        out << "\"lines\":[";
        for (std::size_t j = 0; j < l.lines.size(); ++j) {
            if (j > 0) out << ",";
            out << "{\"timeSeconds\":" << jsonNumber(l.lines[j].timeSeconds) << ",";
            out << "\"text\":" << jsonString(l.lines[j].text) << "}";
        }
        out << "]}";
    }
    out << "],";

    out << "\"cuePoints\":[";
    for (std::size_t i = 0; i < cuePoints.size(); ++i) {
        if (i > 0) out << ",";
        out << "{\"timeSeconds\":" << jsonNumber(cuePoints[i].timeSeconds) << ",";
        out << "\"label\":" << jsonString(cuePoints[i].label) << ",";
        out << "\"sourceFormat\":" << jsonString(cuePoints[i].sourceFormat) << "}";
    }
    out << "],";

    out << "\"broadcast\":{\"present\":" << (broadcast.present ? "true" : "false") << ",";
    out << "\"description\":" << jsonString(broadcast.description) << ",";
    out << "\"originator\":" << jsonString(broadcast.originator) << ",";
    out << "\"originatorReference\":" << jsonString(broadcast.originatorReference) << ",";
    out << "\"originationDate\":" << jsonString(broadcast.originationDate) << ",";
    out << "\"originationTime\":" << jsonString(broadcast.originationTime) << ",";
    out << "\"timeReference\":" << broadcast.timeReference << ",";
    out << "\"version\":" << broadcast.version << ",";
    out << "\"umid\":" << jsonString(broadcast.umid) << ",";
    out << "\"codingHistory\":" << jsonString(broadcast.codingHistory) << ",";
    out << "\"loudnessValueLufs\":" << (broadcast.loudnessValueLufs ? jsonNumber(*broadcast.loudnessValueLufs) : "null") << ",";
    out << "\"loudnessRangeLu\":" << (broadcast.loudnessRangeLu ? jsonNumber(*broadcast.loudnessRangeLu) : "null") << ",";
    out << "\"maxTruePeakDbtp\":" << (broadcast.maxTruePeakDbtp ? jsonNumber(*broadcast.maxTruePeakDbtp) : "null") << ",";
    out << "\"maxMomentaryLufs\":" << (broadcast.maxMomentaryLufs ? jsonNumber(*broadcast.maxMomentaryLufs) : "null") << ",";
    out << "\"maxShortTermLufs\":" << (broadcast.maxShortTermLufs ? jsonNumber(*broadcast.maxShortTermLufs) : "null");
    out << "},";

    out << "\"unmapped\":[";
    for (std::size_t i = 0; i < unmapped.size(); ++i) {
        if (i > 0) out << ",";
        const auto& [key, value] = unmapped[i];
        out << "{\"key\":" << jsonString(key) << ",";
        out << "\"text\":" << jsonString(value.text) << ",";
        out << "\"sourceFormat\":" << jsonString(value.sourceFormat) << ",";
        out << "\"rawKey\":" << jsonString(value.rawKey) << "}";
    }
    out << "],";

    out << "\"fieldConflicts\":[";
    for (std::size_t i = 0; i < fieldConflicts.size(); ++i) {
        if (i > 0) out << ",";
        const auto& [key, value] = fieldConflicts[i];
        out << "{\"field\":" << jsonString(key) << ",";
        out << "\"text\":" << jsonString(value.text) << ",";
        out << "\"sourceFormat\":" << jsonString(value.sourceFormat) << "}";
    }
    out << "],";

    out << "\"diagnostics\":[";
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        if (i > 0) out << ",";
        out << "{\"severity\":" << jsonString(severityName(diagnostics[i].severity)) << ",";
        out << "\"message\":" << jsonString(diagnostics[i].message) << "}";
    }
    out << "]";

    out << "}";
    return out.str();
}

}  // namespace aud::metadata

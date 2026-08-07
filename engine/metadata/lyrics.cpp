#include "lyrics.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>

namespace aud::metadata {

namespace {

// Parses "mm:ss", "mm:ss.xx" or "mm:ss.xxx" into seconds. Returns nullopt if `content` isn't that
// shape (e.g. an `[ar:...]`/`[offset:...]` metadata tag).
std::optional<double> tryParseLrcTimestamp(std::string_view content) {
    std::size_t colon = content.find(':');
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;

    std::string_view minutesPart = content.substr(0, colon);
    std::string_view rest        = content.substr(colon + 1);
    if (rest.empty()) return std::nullopt;

    for (char c : minutesPart) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }

    std::size_t       dot        = rest.find('.');
    std::string_view secondsPart = dot == std::string_view::npos ? rest : rest.substr(0, dot);
    if (secondsPart.empty() || secondsPart.size() > 2) return std::nullopt;
    for (char c : secondsPart) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }

    double fraction = 0.0;
    if (dot != std::string_view::npos) {
        std::string_view fracPart = rest.substr(dot + 1);
        if (fracPart.empty() || fracPart.size() > 3) return std::nullopt;
        for (char c : fracPart) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        }
        double divisor = fracPart.size() == 1 ? 10.0 : (fracPart.size() == 2 ? 100.0 : 1000.0);
        fraction        = std::stod(std::string(fracPart)) / divisor;
    }

    const double minutes = std::stod(std::string(minutesPart));
    const double seconds = std::stod(std::string(secondsPart));
    return minutes * 60.0 + seconds + fraction;
}

// Splits `text` into logical lines on \n, trimming a trailing \r.
std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t                    start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            std::string_view line = text.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lines.push_back(line);
            start = i + 1;
        }
    }
    return lines;
}

}  // namespace

bool looksLikeLrc(const std::string& text) noexcept {
    for (std::string_view line : splitLines(text)) {
        if (line.empty() || line.front() != '[') continue;
        std::size_t close = line.find(']');
        if (close == std::string_view::npos) continue;
        if (tryParseLrcTimestamp(line.substr(1, close - 1)).has_value()) return true;
    }
    return false;
}

std::vector<LyricLine> parseLrc(const std::string& text) {
    std::vector<LyricLine> lines;

    for (std::string_view line : splitLines(text)) {
        std::vector<double> timestamps;
        std::size_t         cursor = 0;

        while (cursor < line.size() && line[cursor] == '[') {
            const std::size_t close = line.find(']', cursor);
            if (close == std::string_view::npos) break;

            if (auto seconds = tryParseLrcTimestamp(line.substr(cursor + 1, close - cursor - 1))) {
                timestamps.push_back(*seconds);
            }
            // Non-timestamp bracket groups ([ar:...], [offset:...], ...) are metadata tags — skip,
            // whether or not they parsed as a timestamp, since either way the lyric text starts
            // after the bracket run.
            cursor = close + 1;
        }

        if (timestamps.empty()) continue;  // not a timed line (metadata tag or plain untagged text)

        std::string lyricText(line.substr(cursor));
        for (double t : timestamps) lines.push_back(LyricLine{t, lyricText});
    }

    std::sort(lines.begin(), lines.end(), [](const LyricLine& a, const LyricLine& b) {
        return a.timeSeconds < b.timeSeconds;
    });
    return lines;
}

Lyrics makeLyricsFromUnsyncedText(std::string text, std::string language, std::string description,
                                   std::string sourceFormat) {
    Lyrics out;
    out.language     = std::move(language);
    out.description  = std::move(description);
    out.sourceFormat = std::move(sourceFormat);

    if (looksLikeLrc(text)) {
        out.synced = true;
        out.lines  = parseLrc(text);
        if (!out.lines.empty()) return out;
        out.synced = false;  // fell through: brackets present but nothing actually parsed
    }

    out.synced = false;
    out.lines.push_back(LyricLine{-1.0, std::move(text)});
    return out;
}

}  // namespace aud::metadata

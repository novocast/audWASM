#include "replaygain.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace aud::metadata {

namespace {

// Trims ASCII whitespace from both ends.
std::string trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

// Parses a leading floating-point number from `text`, tolerating a trailing unit like " dB".
// Returns nullopt if `text` doesn't start with something that looks numeric — never throws (the
// engine is built -fno-exceptions) and never trusts strtod's endptr blindly on attacker-controlled
// tag text without checking it actually consumed something.
std::optional<double> parseLeadingDouble(const std::string& text) {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) return std::nullopt;

    const char* begin = trimmed.c_str();
    char*       end   = nullptr;
    errno            = 0;
    const double value = std::strtod(begin, &end);
    if (end == begin) return std::nullopt;  // nothing parsed
    if (errno == ERANGE) return std::nullopt;
    return value;
}

}  // namespace

std::optional<double> parseReplayGainDb(const std::string& text) { return parseLeadingDouble(text); }

std::optional<double> parseReplayGainPeak(const std::string& text) { return parseLeadingDouble(text); }

std::optional<double> parseItunNormTrackGainDb(const std::string& hexTokens) {
    std::istringstream iss(hexTokens);
    std::string        firstToken;
    if (!(iss >> firstToken) || firstToken.empty()) return std::nullopt;

    // Must be plausible hex (8 hex digits per the format); reject anything else rather than let
    // strtoul silently parse a prefix of garbage.
    for (char c : firstToken) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }

    char*               end   = nullptr;
    errno                     = 0;
    const unsigned long value = std::strtoul(firstToken.c_str(), &end, 16);
    if (end == firstToken.c_str() || errno == ERANGE || value == 0) return std::nullopt;

    // See header comment: best-effort approximation, not a certified figure.
    return 10.0 * std::log10(1000.0 / static_cast<double>(value));
}

}  // namespace aud::metadata

#include "pictures.hpp"

#include <cstring>
#include <initializer_list>

namespace aud::metadata {

namespace {

bool startsWith(std::span<const std::byte> data, std::initializer_list<std::uint8_t> magic) {
    if (data.size() < magic.size()) return false;
    std::size_t i = 0;
    for (std::uint8_t b : magic) {
        if (static_cast<std::uint8_t>(data[i]) != b) return false;
        ++i;
    }
    return true;
}

}  // namespace

std::string detectImageMimeType(std::span<const std::byte> data) noexcept {
    if (startsWith(data, {0xFF, 0xD8, 0xFF})) return "image/jpeg";
    if (startsWith(data, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A})) return "image/png";
    if (startsWith(data, {'G', 'I', 'F', '8', '7', 'a'}) || startsWith(data, {'G', 'I', 'F', '8', '9', 'a'})) {
        return "image/gif";
    }
    if (startsWith(data, {'B', 'M'})) return "image/bmp";
    if (data.size() >= 12 && startsWith(data, {'R', 'I', 'F', 'F'}) &&
        static_cast<char>(data[8]) == 'W' && static_cast<char>(data[9]) == 'E' &&
        static_cast<char>(data[10]) == 'B' && static_cast<char>(data[11]) == 'P') {
        return "image/webp";
    }
    return {};
}

bool extractPicture(std::span<const std::byte> data, const std::string& declaredMimeType, PictureType type,
                     std::string description, const std::string& sourceFormat, Picture& out,
                     std::vector<Diagnostic>& diagnostics) {
    if (data.size() > kMaxPictureBytes) {
        diagnostics.push_back({Severity::Warning, sourceFormat + ": embedded picture (" +
                                                       std::to_string(data.size()) +
                                                       " bytes) exceeds the 32MB cap; skipped"});
        return false;
    }

    out.data             = std::vector<std::byte>(data.begin(), data.end());
    out.declaredMimeType = declaredMimeType;
    out.detectedMimeType = detectImageMimeType(data);
    out.mimeMismatch     = !out.detectedMimeType.empty() && !declaredMimeType.empty() &&
                        out.detectedMimeType != declaredMimeType;
    out.type             = type;
    out.description      = std::move(description);
    out.sourceFormat      = sourceFormat;

    if (out.mimeMismatch) {
        diagnostics.push_back({Severity::Warning, sourceFormat + ": picture declared MIME type '" +
                                                       declaredMimeType + "' but magic bytes indicate '" +
                                                       out.detectedMimeType + "'"});
    }
    if (out.detectedMimeType.empty() && !data.empty()) {
        diagnostics.push_back({Severity::Info, sourceFormat + ": embedded picture has unrecognised magic bytes"});
    }

    return true;
}

}  // namespace aud::metadata

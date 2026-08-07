#pragma once

// Cover-art extraction helpers shared by every tag parser that carries embedded pictures (ID3
// APIC, FLAC PICTURE / Vorbis METADATA_BLOCK_PICTURE, MP4 covr). See M15 "Cover art":
// image bytes are never decoded in C++ (handed to the browser as a heap view); this file's job is
// just to lift the bytes out of the tag safely and record what the tag *claims* vs what the magic
// bytes *say*.

#include <cstddef>
#include <span>
#include <string>

#include "metadata.hpp"

namespace aud::metadata {

// M15 "sanity-cap image size (reject >32 MB) before handing anything to the browser" — applies
// equally to the allocation itself: this is checked against the *declared* length before any copy
// is made, so a 200MB declared length in a 3MB file never allocates 200MB (or even touches memory)
// to find out it's bogus.
inline constexpr std::size_t kMaxPictureBytes = 32u * 1024u * 1024u;

// Sniffs `data`'s magic bytes and returns a MIME type string ("image/jpeg", "image/png",
// "image/gif", "image/bmp", "image/webp"), or an empty string if unrecognised.
[[nodiscard]] std::string detectImageMimeType(std::span<const std::byte> data) noexcept;

// Builds a Picture from tag-embedded bytes plus the tag's own claimed MIME type. Returns false
// (and appends a diagnostic) if `declaredLength` is inconsistent with `available` (the remaining
// bytes in the tag) or exceeds kMaxPictureBytes — in both cases nothing is copied.
[[nodiscard]] bool extractPicture(std::span<const std::byte> data, const std::string& declaredMimeType,
                                   PictureType type, std::string description, const std::string& sourceFormat,
                                   Picture& out, std::vector<Diagnostic>& diagnostics);

}  // namespace aud::metadata

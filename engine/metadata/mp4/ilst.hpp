#pragma once

// MP4/M4A `ilst` (iTunes-style metadata atom list) — see M15 "MP4 / M4A" row and "MP4 parsing
// note". Built on top of box_reader.hpp's general box walker.

#include <span>

#include "../tag_set.hpp"

namespace aud::metadata::mp4 {

// `bytes` should be the whole file (moov/udta/meta/ilst can, in principle, appear anywhere a
// conformant box walk reaches, though in practice always inside top-level `moov`).
[[nodiscard]] TagSet parseIlst(std::span<const std::byte> fileBytes);

}  // namespace aud::metadata::mp4

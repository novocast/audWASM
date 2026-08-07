#pragma once

// General-purpose, bounds-checked MP4/ISOBMFF box (atom) walker — see M15 "MP4 parsing note":
// built as a standalone reusable component rather than metadata-specific code, since a future
// native AAC decode path (M02) needs exactly the same box iteration to find `mdat`/`moov`/`trak`.
//
// Deliberately minimal: iterates *direct* children of a byte span, handling the 32-bit-size,
// 64-bit-largesize, and size-extends-to-end-of-data cases. Recursing into container boxes (moov,
// udta, meta, ilst, ...) is the caller's job — different boxes have different internal shapes
// (`meta` has a 4-byte version+flags prefix before its children; `stsd` has a different one still),
// so a one-size-fits-all recursive walk would either be wrong for some box type or need as much
// per-box special-casing as just calling forEachBox again from the caller.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace aud::metadata::mp4 {

struct Box {
    std::string                type;  // 4-character fourcc, e.g. "moov", "ilst", "data"
    std::span<const std::byte> body;  // payload only — the 8/16-byte header is not included
};

// Calls `visit` for each direct child box of `bytes`, in order. Stops silently (without invoking
// `visit` again) at the first box whose declared size is inconsistent (too small to be a valid
// header, or overruns what's left of `bytes`) — a truncated/corrupt tail is reported as "no more
// boxes", never as an out-of-bounds read. Returns the number of boxes successfully visited.
std::size_t forEachBox(std::span<const std::byte> bytes, const std::function<void(const Box&)>& visit);

}  // namespace aud::metadata::mp4

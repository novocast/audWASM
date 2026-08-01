#pragma once

#include <cstdint>

namespace aud {

using Sample       = float;         // normalised, nominally [-1, 1], may exceed for headroom
using FrameIndex    = std::int64_t; // sample frames; -1 is a valid "none" sentinel
using ChannelIndex  = std::uint32_t;
using SampleRate    = std::uint32_t;

inline constexpr FrameIndex kNoFrame = -1;

struct TimeRange {
    double startSeconds = 0.0;
    double endSeconds   = 0.0;

    [[nodiscard]] double durationSeconds() const noexcept { return endSeconds - startSeconds; }
};

// Half-open [begin, end) in sample frames.
struct FrameRange {
    FrameIndex begin = 0;
    FrameIndex end   = 0;

    [[nodiscard]] FrameIndex frameCount() const noexcept { return end - begin; }
    [[nodiscard]] bool       empty() const noexcept { return end <= begin; }
    [[nodiscard]] bool       contains(FrameIndex frame) const noexcept {
        return frame >= begin && frame < end;
    }
};

enum class ChannelLayout : std::uint8_t {
    Mono,
    Stereo,
    Unknown,
};

}  // namespace aud

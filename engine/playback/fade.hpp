#pragma once

// Cosine ramp helpers used for every discontinuity (seek, loop wrap, pause, stop, resume). See
// M03 "Click-free everything": a hard gain step at a waveform discontinuity is audible as a click;
// a short cosine (raised-cosine / "hann half") ramp between the two gains removes it cheaply.

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "../util/audio_types.hpp"

namespace aud::playback {

inline constexpr double kPi = 3.14159265358979323846;

// Applies an in-place cosine-eased ramp from `startGain` to `endGain` across `frames` samples of
// one channel. `frames == 1` snaps straight to `endGain` (nothing to interpolate).
inline void applyCosineRamp(Sample* samples, std::size_t frames, float startGain, float endGain) noexcept {
    if (frames == 0) {
        return;
    }
    if (frames == 1) {
        samples[0] *= endGain;
        return;
    }
    const double step = kPi / static_cast<double>(frames - 1);
    for (std::size_t i = 0; i < frames; ++i) {
        // 0 -> 1 raised-cosine ease, applied identically regardless of ramp direction.
        const double eased = 0.5 - 0.5 * std::cos(static_cast<double>(i) * step);
        const float  gain  = static_cast<float>(startGain + (endGain - startGain) * eased);
        samples[i] *= gain;
    }
}

// Converts a millisecond duration to a whole frame count at `sampleRate`, rounding to nearest.
[[nodiscard]] inline std::size_t framesForMilliseconds(double milliseconds, SampleRate sampleRate) noexcept {
    if (milliseconds <= 0.0 || sampleRate == 0) {
        return 0;
    }
    return static_cast<std::size_t>(milliseconds * 0.001 * static_cast<double>(sampleRate) + 0.5);
}

// One render quantum (128 frames) worth of milliseconds at 44.1/48k is ~2.7-2.9ms; used as the
// default mute duration when dropping buffered content on a seek (M03 "Seeking" procedure step 2).
inline constexpr std::size_t kRenderQuantumFrames = 128;

}  // namespace aud::playback

#pragma once

// The per-bin waveform summary. See M04 "What a waveform bin is": min/max (not a single absolute
// peak) because asymmetric waveforms are real and informative, RMS in the same bin because
// computing it in a second pass over the PCM would double the read bandwidth, absPeak precomputed
// because it's read once per pixel per frame in the renderer's hot loop (M17).

#include <cstddef>

#include "../util/audio_buffer.hpp"
#include "../util/audio_types.hpp"

namespace aud::waveform {

struct WaveformBin {
    Sample min     = 0.0f;  // most negative sample in the bin
    Sample max     = 0.0f;  // most positive sample in the bin
    Sample rms     = 0.0f;  // sqrt(mean(x^2)) over the bin
    Sample absPeak = 0.0f;  // max(|min|, |max|)
};
static_assert(sizeof(WaveformBin) == 16, "WaveformBin must stay 16 bytes, 4-float aligned (M04)");
static_assert(alignof(WaveformBin) == 4);

// Level-0 (finest) bin size. Power of two so the M05 mipmap reduction is an exact 2:1 fold with no
// partial-bin bookkeeping.
inline constexpr std::size_t kBaseBinFrames = 256;
static_assert((kBaseBinFrames & (kBaseBinFrames - 1)) == 0, "kBaseBinFrames must be a power of two");

// This is the invariant that removes all carry-over state between chunks (M04 "Streaming
// generation"): every AudioBuffer chunk starts and ends exactly on a bin boundary, so chunks can
// be reduced independently, out of order, or in parallel, with no bin ever spanning two chunks.
static_assert(AudioBuffer::kChunkFrames % kBaseBinFrames == 0,
              "AudioBuffer::kChunkFrames must be an exact multiple of kBaseBinFrames");

inline constexpr std::size_t kBinsPerChunk = AudioBuffer::kChunkFrames / kBaseBinFrames;

}  // namespace aud::waveform

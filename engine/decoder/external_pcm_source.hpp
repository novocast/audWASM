#pragma once

// Ingestion path for PCM decoded outside the engine — browser-side AAC/M4A via
// AudioContext.decodeAudioData()/WebCodecs (M02), and later microphone/stream input (M21). The
// frontend hands over planar float32 channel data it already has; this just validates and appends
// it to an AudioBuffer through the same path a native decoder would use, so downstream code cannot
// tell the difference.

#include <span>

#include "../util/audio_buffer.hpp"
#include "../util/audio_types.hpp"
#include "../util/result.hpp"

namespace aud::decoder {

struct ExternalPcmMetadata {
    SampleRate  sampleRate = 0;
    bool        resampledByBrowser = false;  // true if the browser decoded at a different rate
                                               // than the file's native rate and we couldn't avoid it
};

// Appends one batch of externally-decoded planar PCM (one span per channel, all equal length) into
// `buffer`. `buffer` must already have been created with the same channel count and sample rate as
// `metadata`. Channel-at-a-time transfer from JS (each Float32Array released as it's copied) keeps
// the transient memory doubling described in M02 bounded to one channel at a time.
Result<void> ingestExternalPcm(AudioBuffer& buffer, std::span<const std::span<const Sample>> planarChannels,
                                std::size_t frameCount);

}  // namespace aud::decoder

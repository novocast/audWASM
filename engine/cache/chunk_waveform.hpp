#pragma once

// Serialization helpers for waveform (M04) analysis results.
// Converts WaveformStore ↔ WVPY chunk payload.

#include <cstdint>
#include <vector>

#include "../waveform/waveform_store.hpp"
#include "../util/result.hpp"

namespace aud::cache::chunks {

// Serialize a WaveformStore to the WVPY chunk payload.
// This is the multi-level mipmap pyramid for efficient waveform display.
[[nodiscard]] Result<std::vector<uint8_t>> serializeWaveformPyramid(
    const aud::waveform::WaveformStore& store
);

// Deserialize a WVPY chunk payload into a WaveformStore.
[[nodiscard]] Result<void> deserializeWaveformPyramid(
    std::span<const uint8_t> payload,
    aud::waveform::WaveformStore& store
);

// Waveform pyramid has no parameters (it's fully derived from the audio), so the param hash is 0.
[[nodiscard]] constexpr uint64_t hashWaveformParams() {
    return 0;
}

}  // namespace aud::cache::chunks

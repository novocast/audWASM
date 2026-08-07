#pragma once

// Serialization helpers for loudness (M08) analysis results.
// Converts LoudnessResult ↔ LUFS chunk payload.

#include <cstdint>
#include <vector>

#include "../analysis/loudness/loudness_analyzer.hpp"
#include "../util/result.hpp"

namespace aud::cache::chunks {

// Serialize a LoudnessResult to the LUFS chunk payload.
[[nodiscard]] Result<std::vector<uint8_t>> serializeLoudness(
    const aud::loudness::LoudnessResult& result
);

// Deserialize a LUFS chunk payload into a LoudnessResult.
[[nodiscard]] Result<void> deserializeLoudness(
    std::span<const uint8_t> payload,
    aud::loudness::LoudnessResult& result
);

// Compute the parameter hash for LoudnessConfig.
[[nodiscard]] uint64_t hashLoudnessConfig(const aud::loudness::LoudnessConfig& config);

}  // namespace aud::cache::chunks

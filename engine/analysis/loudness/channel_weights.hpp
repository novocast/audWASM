#pragma once

// BS.1770 channel weighting: 1.0 for L/R/C, 1.41 (+1.5 dB) for the surround pair Ls/Rs, and the
// LFE channel excluded entirely (weight 0 — and skipped by the K-weighting filter altogether,
// since a weight-0 channel can never affect the sum). This requires knowing channel *layout*, not
// just channel count.
//
// M02 gap (documented in M08's design, not fixed here): AudioSpec (engine/analysis/analyzer.hpp)
// carries only a channel count, no container-supplied layout mask — WAV's WAVE_FORMAT_EXTENSIBLE
// dwChannelMask is not yet threaded through decode_session/wav_decoder up to the Analyzer
// boundary. Two entry points are provided so the wiring can land independently of this file:
//   - resolveChannelRolesByCount(): the documented fallback assumption (SMPTE-standard ordering
//     for 1/2/6 channels), used whenever no mask is available. Emits an AUD_LOG_WARN for channel
//     counts outside the well-known set, per M08's risk table ("never silently guess").
//   - resolveChannelRolesFromWavMask(): ready to consume a WAVE_FORMAT_EXTENSIBLE mask once M02
//     exposes one; unused until that wiring exists.

#include <cstdint>
#include <optional>
#include <vector>

#include "../../util/audio_types.hpp"

namespace aud::loudness {

enum class ChannelRole : std::uint8_t {
    FrontLeft,
    FrontRight,
    FrontCenter,
    Lfe,
    BackLeft,   // surround / rear-left — 1.41 weight
    BackRight,  // surround / rear-right — 1.41 weight
    Other,      // unrecognised position; falls back to 1.0 (documented assumption)
};

[[nodiscard]] double weightForRole(ChannelRole role) noexcept;

// SMPTE/WAVE_FORMAT_EXTENSIBLE speaker-mask bits relevant to BS.1770 weighting (low bits only).
enum class WavSpeakerBit : std::uint32_t {
    FrontLeft   = 0x1,
    FrontRight  = 0x2,
    FrontCenter = 0x4,
    LowFrequency = 0x8,
    BackLeft    = 0x10,
    BackRight   = 0x20,
    SideLeft    = 0x200,
    SideRight   = 0x400,
};

// Fallback used when no container layout mask is available. Documented assumption (never a
// silent guess — callers should surface the returned `usedFallback` to the UI per M08's risk
// table): 1 channel -> centre; 2 -> L/R; 6 -> 5.1 (L R C LFE Ls Rs, standard WAV order); any other
// count -> every channel weighted 1.0 (logs a warning).
struct ChannelWeightResolution {
    std::vector<double> weights;       // one per channel, in container order
    bool                usedFallback;  // true whenever no explicit layout mask was available
};

[[nodiscard]] ChannelWeightResolution resolveChannelRolesByCount(ChannelIndex channelCount);

// Resolves weights from an explicit WAVE_FORMAT_EXTENSIBLE-style channel mask (bit order per
// WavSpeakerBit above, in the order Microsoft defines — i.e. increasing bit value). Channels
// present in `channelCount` but not represented by any set bit fall back to weight 1.0.
[[nodiscard]] ChannelWeightResolution resolveChannelRolesFromWavMask(ChannelIndex channelCount,
                                                                      std::uint32_t channelMask);

}  // namespace aud::loudness

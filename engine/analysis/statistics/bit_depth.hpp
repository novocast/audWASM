#pragma once

// Effective bit-depth detection (M09 "Effective bit depth"): finds the largest k such that every
// sample, converted back to the source integer domain, is a multiple of 2^k. Also carries a
// hedged dither heuristic — never a hard assertion, always "probable" with a confidence, per M09's
// risk table ("Dither detection false positives" -> "Hedged wording + confidence").
//
// Streaming, single-pass: the OR-of-quantised-values trick this relies on (trailing zero count of
// the bitwise OR of every sample's integer-domain value) is associative, so it accumulates across
// chunks exactly like the rest of StatisticsAnalyzer's state, with no need to hold the file in
// memory (bit_depth.cpp's BitDepthAccumulator::process()).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "../../util/audio_types.hpp"

namespace aud::statistics {

struct BitDepthResult {
    // Container's nominal bit depth as decoded (0 means "float source, no integer container").
    std::uint32_t containerBitDepth = 0;

    // Detected resolution of the actual content. For an integer container this is
    // containerBitDepth - k. For a float source with a detected quantisation grid, this is the
    // grid's bit depth; std::nullopt means "float, no quantisation detected".
    std::optional<std::uint32_t> effectiveBitDepth;

    bool   ditherLikely     = false;
    double ditherConfidence = 0.0;  // [0, 1], meaningful only when ditherLikely

    // Human-readable summary, e.g. "24-bit container, 16-bit effective content" or
    // "float, no quantisation detected" — built here so the CLI/JSON/UI never have to re-derive
    // the wording (and risk disagreeing with each other about it).
    [[nodiscard]] std::string describe() const;
};

// Candidate quantisation depths tried for float sources (M09: "common when a 16-bit file was
// converted to float").
inline constexpr std::array<std::uint32_t, 4> kFloatQuantisationCandidates{8, 16, 20, 24};

class BitDepthAccumulator {
public:
    // `containerBitDepth` is the source integer container's depth (e.g. 16, 24), or 0 if the
    // source was float (WAV IEEE float, etc.) — see decode_session/StreamInfo::bitDepth (M02).
    void begin(std::uint32_t containerBitDepth) noexcept;

    void process(std::span<const Sample> samples) noexcept;

    [[nodiscard]] BitDepthResult finish() const;

private:
    std::uint32_t m_containerBitDepth = 0;
    double        m_intScale          = 0.0;  // 2^(containerBitDepth - 1), for integer containers

    std::uint64_t m_orAccumulator  = 0;
    std::uint64_t m_nonZeroSamples = 0;
    std::uint64_t m_totalSamples   = 0;

    bool          m_hasPrevInt = false;
    std::int64_t  m_prevInt    = 0;
    std::uint64_t m_lsbToggles = 0;
    std::uint64_t m_transitions = 0;

    // Per-candidate max round-trip error, for float sources only.
    std::array<double, kFloatQuantisationCandidates.size()> m_maxError{};
};

// Convenience wrapper for callers (and tests) that already have the whole channel in memory.
[[nodiscard]] BitDepthResult detectEffectiveBitDepth(std::span<const Sample> samples, std::uint32_t containerBitDepth);

}  // namespace aud::statistics

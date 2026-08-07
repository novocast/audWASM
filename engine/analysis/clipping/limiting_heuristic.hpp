#pragma once

// M11 "Soft clipping / limiting": modern masters are limited rather than hard-clipped, so there
// may be no flat runs at all (ClipDetectorAnalyzer's Digital/OverFullScale/NearClip trackers find
// nothing) while the material is still crushed. This is a separate, softer signal:
//   - Flat-top ratio: fraction of samples within 0.5 dB of that channel's peak. High values
//     indicate heavy limiting.
//   - Consecutive-equal-sample plateau histogram: limiters produce characteristic short plateaus
//     even without hard clipping.
// Reported as an advisory ("heavy limiting detected") with the supporting numbers, never as an
// error (M11: this overlaps with M09's crest factor; cross-link rather than duplicate — this
// accumulator intentionally carries none of M09's crest-factor/RMS numbers itself).
//
// Single-pass and streaming, like every other M08/M09 accumulator: samples are classified into a
// fixed-resolution absolute-dBFS histogram as they arrive (the channel's peak isn't known for sure
// until it's all been seen), and flat-top ratio is derived from that histogram in finish() once the
// caller supplies the final per-channel peaks — summing the histogram buckets that fall within
// kFlatTopWindowDb of each channel's peak. Trades a little precision (0.1 dB bucket granularity)
// for staying single-pass.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "../../util/audio_types.hpp"

namespace aud::clipping {

struct LimitingHeuristicResult {
    double        flatTopRatio        = 0.0;  // fraction of samples (summed over channels) within kFlatTopWindowDb of their channel's peak
    double        meanPlateauLength   = 0.0;  // mean run length of consecutive bit-identical samples, over runs of length >= 2
    bool          heavyLimitingLikely = false;
    std::uint64_t samplesConsidered   = 0;
};

class LimitingHeuristicAccumulator {
public:
    void begin(ChannelIndex channels);

    void process(ChannelIndex channel, std::span<const Sample> samples) noexcept;

    // `peakLinearPerChannel` must have one entry per channel — the exact per-channel peaks (e.g.
    // from the same clipping pass's ceiling trackers, or M09 statistics).
    [[nodiscard]] LimitingHeuristicResult finish(std::span<const double> peakLinearPerChannel) const;

private:
    static constexpr double      kBucketWidthDb   = 0.1;
    static constexpr double      kHistogramFloorDb = -60.0;  // samples quieter than this can never be "near peak"
    static constexpr double      kFlatTopWindowDb = 0.5;     // M11: "within 0.5 dB of the peak"
    // Decision: 30%. A sine's curvature alone already puts a surprisingly large slice of every
    // cycle within 0.5 dB of its own peak (a pure, unmodulated tone measures ~21% by this metric —
    // sin(theta) lingers near 1 for a wide angular range around theta=90 degrees), so anything much
    // lower would flag ordinary sustained tones as "limited". Genuinely brick-walled masters
    // typically measure 60-95%+ (most of the file, not just the top of each cycle, sits at the
    // ceiling). Advisory only (M11: never an error), so a false positive just shows an extra badge,
    // not a broken result.
    static constexpr double      kHeavyLimitingFlatTopThreshold = 0.3;

    [[nodiscard]] static std::size_t bucketCount() noexcept {
        return static_cast<std::size_t>(-kHistogramFloorDb / kBucketWidthDb) + 1;
    }
    [[nodiscard]] static std::size_t bucketForDbfs(double dbfs) noexcept;

    ChannelIndex m_channels = 0;

    // Per channel: [bucket] -> sample count. Buckets are absolute dBFS, not relative to the
    // (not-yet-known) peak — see file comment.
    std::vector<std::vector<std::uint64_t>> m_histogram;

    // Plateau tracking, per channel.
    std::vector<Sample>       m_prevValue;
    std::vector<bool>         m_hasPrev;
    std::vector<std::uint64_t> m_runLength;
    std::uint64_t              m_plateauRunTotalLength = 0;  // sum of lengths over runs >= 2
    std::uint64_t              m_plateauRunCount       = 0;  // count of runs >= 2

    std::uint64_t m_samplesConsidered = 0;

    void flushPlateau(ChannelIndex ch) noexcept;
};

}  // namespace aud::clipping

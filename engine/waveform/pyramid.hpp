#pragma once

// The M05 mipmap pyramid: level 0 is WaveformStore's existing per-chunk reduction, and every
// level above it is an exact 2:1 fold of the level below (see M05 "The pyramid"). Doubling the
// memory of level 0 buys O(1)-cost zoom at every scale, because query() always aggregates from
// whichever level is closest to (but finer than) the requested resolution rather than re-scanning
// PCM.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../util/audio_types.hpp"
#include "waveform_bin.hpp"

namespace aud::waveform {

// Folds two adjacent bins into their parent. Min/max/absPeak fold exactly regardless of the
// children's frame counts — peaks are never lost at any zoom level. RMS is combined by recovering
// each child's sum-of-squares (rms^2 * frameCount) before summing, which is exact when the two
// children cover equal frame counts and correct (not just an equal-weight approximation) when they
// don't — the case that matters is a level's trailing bin, which may be shorter than its siblings
// (M05 "The fold is exact and trivially correct").
[[nodiscard]] WaveformBin fold(const WaveformBin& a, std::uint32_t framesA, const WaveformBin& b,
                                std::uint32_t framesB) noexcept;

// Sentinel returned by selectLevel() when the requested resolution is finer than level 0 — the
// caller must fall back to reading raw PCM directly (M05 "Below level 0").
inline constexpr std::uint32_t kRawPcmLevel = ~std::uint32_t{0};

// Picks the finest pyramid level whose bin size does not exceed `framesPerRequestedBin`, clamped
// to `maxLevel` (M05 "Level selection & the fractional-zoom problem"). Deliberately floors rather
// than rounds: an under-coarse level aggregated down looks correct; a coarse level interpolated up
// loses peaks and looks blocky. Returns kRawPcmLevel if framesPerRequestedBin is finer than level 0.
[[nodiscard]] std::uint32_t selectLevel(std::uint64_t framesPerRequestedBin, std::uint32_t maxLevel) noexcept;

// Aggregates the source bins of one pyramid level covering [0, srcBins.size()) — each `framesPerSrcBin`
// frames wide except the last, which covers `trailingFrameCount` — into a single output bin spanning
// the frame range [frameStart, frameEnd). Every source bin that even partially overlaps the range
// contributes fully to min/max/absPeak (a peak that only touches the edge of the range still belongs
// in it); RMS is area-weighted by each source bin's covered-frame overlap (M05 "aggregate with exact
// fractional coverage, not nearest-neighbour").
[[nodiscard]] WaveformBin aggregateRange(std::span<const WaveformBin> srcBins, std::uint32_t framesPerSrcBin,
                                          std::uint32_t trailingFrameCount, std::uint64_t frameStart,
                                          std::uint64_t frameEnd) noexcept;

// Owns the full mipmap pyramid (level 0 upward) for every channel of one waveform "variant"
// (per-channel, mono-sum, or one of mid/side — see WaveformStore's ChannelSelector). Built
// incrementally alongside decode: appendLevel0Bins() feeds newly-reduced level-0 bins for one
// channel and cascades the fold upward as far as the data allows (M05 "Construction" — when level L
// gains 2 complete bins, level L+1 gains 1).
class WaveformPyramid {
public:
    void reset(ChannelIndex channelCount);

    // `bins` are freshly reduced level-0 bins (see reduce.hpp's reduceToBins) covering exactly
    // `framesCovered` consecutive PCM frames for channel `ch`, appended after everything already
    // fed for that channel. `framesCovered` may be less than `bins.size() * kBaseBinFrames` only if
    // this is the last call ever made for `ch` (M04's chunk-alignment invariant guarantees only the
    // very last bin of the whole channel can be partial).
    void appendLevel0Bins(ChannelIndex ch, std::span<const WaveformBin> bins, std::size_t framesCovered);

    // Packs every channel's levels into one contiguous allocation with an offset table (M05
    // "Storage layout"), and drops any levels built beyond the point where a level would have <= 32
    // bins (M05 "Build levels until a level has <= 32 bins"). Idempotent; call once decode (or the
    // one-shot batch reduction for a lazily-computed variant) is complete. Querying before finalize()
    // is still correct — it just sees whatever levels have been built so far, unpacked.
    void finalize();

    [[nodiscard]] ChannelIndex channelCount() const noexcept;
    [[nodiscard]] std::uint32_t levelCount(ChannelIndex ch) const noexcept;
    [[nodiscard]] std::uint32_t maxLevel(ChannelIndex ch) const noexcept;
    [[nodiscard]] static std::uint32_t framesPerBin(std::uint32_t level) noexcept {
        return static_cast<std::uint32_t>(kBaseBinFrames) << level;
    }

    [[nodiscard]] std::span<const WaveformBin> level(ChannelIndex ch, std::uint32_t level) const noexcept;
    [[nodiscard]] std::uint32_t trailingFrameCount(ChannelIndex ch, std::uint32_t level) const noexcept;

    // Total bins stored across every level for one channel — level 0's count plus the (small)
    // overhead of every level above it. Used to verify the <2.05x memory bound.
    [[nodiscard]] std::size_t totalBinCount(ChannelIndex ch) const noexcept;

private:
    struct ChannelState {
        // Pre-finalize: one growing vector per level. Post-finalize: concatenated into `packed`
        // with `offsets` marking each level's region, freeing `levels`/`trailing` down to just the
        // trailing-frame-count bookkeeping still needed by query().
        std::vector<std::vector<WaveformBin>> levels;
        std::vector<std::uint32_t>            trailing;  // frame count of each level's last bin

        std::vector<WaveformBin>   packed;
        std::vector<std::uint32_t> offsets;  // size levelCount + 1 once finalized
        bool                       finalized = false;
    };

    void cascade(ChannelState& state);

    std::vector<ChannelState> m_channels;
};

}  // namespace aud::waveform

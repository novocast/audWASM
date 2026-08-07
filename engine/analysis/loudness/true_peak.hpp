#pragma once

// ITU-R BS.1770-4 Annex 2 true peak: 4x (spec minimum) polyphase-oversampled inter-sample peak
// estimation, measured on the unweighted signal per channel. M08's decision: reuse M03's
// Kaiser-windowed polyphase Resampler rather than a bespoke FIR — same math, a pure upsample
// ratio instead of a rate-conversion ratio. Also offers 8x/16x "high precision" modes; the
// oversampling factor actually used is always reported alongside the number (a 4x reading
// under-reads worst-case true peak by up to ~0.6 dB, and disagreeing silently with a
// higher-precision reading elsewhere would be worse than not offering one).
//
// Sample peak (plain |x| max, no oversampling) is tracked alongside for free since it falls out
// of the same per-chunk scan.
//
// M11 extends this pass (rather than adding a second oversampler — see M11's risk table,
// "Duplicating M08's oversampling work") to optionally record every inter-sample-peak excursion
// above a configurable dBTP ceiling, not just the running maximum: enableIspEventCapture() turns
// this on; the marginal cost over the existing max-tracking scan is one extra comparison and an
// occasional push_back, exactly as M11 calls for. Off by default (empty vector, zero extra cost)
// so M08's own callers (LoudnessAnalyzer, its tests) are unaffected.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../playback/resampler.hpp"
#include "../../util/audio_types.hpp"

namespace aud::loudness {

// One inter-sample-peak excursion above the configured ceiling. `samplePeakNearbyDbfs` is the
// plain (non-oversampled) sample peak within the same source-domain neighbourhood, carried along
// so a caller can show "samples read -0.3 dBFS but the true peak is +0.8 dBTP" (M11's "Output"
// section) without a second pass.
struct IspEvent {
    FrameIndex   frame                 = kNoFrame;  // source-domain frame (oversampled index / factor)
    ChannelIndex channel                = 0;
    double       truePeakDbtp           = 0.0;
    double       samplePeakNearbyDbfs   = 0.0;
};

class TruePeakMeter {
public:
    // `oversampling` must be 4, 8 or 16 per BS.1770-4 Annex 2 / M08's decision.
    TruePeakMeter(SampleRate sourceRate, ChannelIndex channels, std::uint32_t oversampling);

    // Enables recording of every oversampled excursion above `thresholdDbtp` (M11's ISP detector;
    // default 0 dBTP) into ispEvents(), capped at `maxEvents` (0 = unbounded — callers doing their
    // own capping, e.g. via a BoundedTopN over the returned list, may prefer that instead). Must be
    // called before the first process().
    void enableIspEventCapture(double thresholdDbtp, std::size_t maxEvents = 0) noexcept;

    // Feeds one chunk of unweighted planar samples (same shape as ChunkView::channels). Updates
    // the running true-peak and sample-peak maxima; safe to call repeatedly across a stream.
    void process(std::span<const std::span<const Sample>> planarChannels);

    // Flushes the oversampling filter's lookahead tail. Call once after the last process().
    void finish();

    [[nodiscard]] double truePeakDbtpFor(ChannelIndex channel) const noexcept;
    [[nodiscard]] double samplePeakDbfsFor(ChannelIndex channel) const noexcept;
    [[nodiscard]] double truePeakDbtpOverall() const noexcept;
    [[nodiscard]] FrameIndex truePeakFrame() const noexcept { return m_peakFrame; }
    [[nodiscard]] std::uint32_t oversamplingFactor() const noexcept { return m_factor; }

    // Empty unless enableIspEventCapture() was called. In source-frame order per process()/finish()
    // call, but not globally re-sorted across channels.
    [[nodiscard]] const std::vector<IspEvent>& ispEvents() const noexcept { return m_ispEvents; }
    // True count of excursions seen, even once maxEvents caps the stored list (M11: "never
    // silently truncate; always show the true count").
    [[nodiscard]] std::size_t ispEventCountTotal() const noexcept { return m_ispEventCountTotal; }

private:
    void scanOversampledOutput(std::span<const std::span<Sample>> out, std::size_t framesProduced);

    ChannelIndex          m_channels;
    std::uint32_t         m_factor;
    playback::Resampler   m_resampler;

    std::vector<double>   m_truePeakLinear;    // per channel, linear amplitude
    std::vector<double>   m_samplePeakLinear;  // per channel, linear amplitude

    double     m_overallPeakLinear = 0.0;
    FrameIndex m_peakFrame         = kNoFrame;
    double     m_outputFrameCount  = 0.0;  // running count of oversampled frames produced so far

    bool                   m_ispCaptureEnabled  = false;
    double                 m_ispThresholdLinear = 1.0;  // 0 dBTP by default once enabled
    std::size_t            m_ispMaxEvents       = 0;    // 0 = unbounded
    std::vector<IspEvent>  m_ispEvents;
    std::size_t            m_ispEventCountTotal = 0;
    std::vector<double>    m_chunkSamplePeakLinear;  // per channel, whole-chunk peak of the most recent process() call

    std::vector<std::vector<Sample>> m_scratch;      // per-channel oversampled output, reused
    std::vector<std::span<Sample>>   m_scratchSpans;
};

}  // namespace aud::loudness

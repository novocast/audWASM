#pragma once

// M11's clipping detector — see documentation/tasks/M11-clipping-detection.md. A streaming
// Analyzer (unlike M10's SilenceDetector, which runs over an already-reduced series): flat-run
// detection needs sample-accurate access to PCM, which only exists chunk-by-chunk here, so this
// follows StatisticsAnalyzer/LoudnessAnalyzer's shape instead.
//
// Three kinds of clipping, three detectors, run in the same single pass over PCM:
//   - Digital: consecutive samples at/beyond the *source-format-aware* ceiling (M11's single most
//     likely bug: a hardcoded 1.0 ceiling misses every clipped integer file, because M02 converts
//     n-bit integers to float by dividing by 2^(n-1), so the positive maximum lands slightly below
//     1.0 — e.g. 32767/32768 = 0.99997 for 16-bit). Integer sources only.
//   - OverFullScale: float sources exceeding 0 dBFS (linear 1.0). A float file at 1.4 is not
//     "clipped" — it has no ceiling — but it *will* clip on export to any integer format, so this
//     is reported as a distinct, differently-worded finding rather than conflated with Digital.
//   - NearClip: samples within `nearClipDbfs` of full scale (1.0), regardless of source format — a
//     headroom warning independent of the digital-vs-float distinction above.
//   - InterSamplePeak: M08's TruePeakMeter, oversampled, tracking every excursion above 0 dBTP
//     rather than just the running maximum (extends the existing pass; see true_peak.hpp — M11's
//     risk table explicitly calls out not duplicating this machinery).
//
// Order of operations, per kind, per channel (mirrors M10's silence pipeline):
//   1. Classify each sample against that kind's threshold (source-aware ceiling for Digital; 1.0
//      for OverFullScale; nearClipDbfs's linear threshold for NearClip).
//   2. Run-length encode consecutive "over threshold" samples.
//   3. Merge runs separated by a gap shorter than mergeGapSamples.
//   4. Discard runs shorter than the kind's minRunSamples.
// Digital/OverFullScale/NearClip run independently (a sample can be both NearClip and, once it
// crosses the harder ceiling, Digital/OverFullScale — these are different findings, not mutually
// exclusive).

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../util/audio_types.hpp"
#include "../../util/bounded_top_n.hpp"
#include "../analyzer.hpp"
#include "../loudness/true_peak.hpp"

namespace aud::clipping {

class LimitingHeuristicAccumulator;  // limiting_heuristic.hpp — forward-declared to keep this header light

enum class ClipKind : std::uint8_t {
    Digital,          // integer-domain full-scale run
    OverFullScale,     // float source exceeding 0 dBFS (would clip on export)
    NearClip,
    InterSamplePeak,
};

inline constexpr std::size_t kClipKindCount = 4;

struct ClipEvent {
    FrameRange    range;
    double        startSeconds = 0.0;
    double        endSeconds   = 0.0;
    ChannelIndex  channel      = 0;
    ClipKind      kind         = ClipKind::Digital;
    double        peakValue    = 0.0;  // linear; may exceed 1.0
    double        peakDbfs     = 0.0;  // or dBTP for InterSamplePeak
    std::uint32_t sampleCount  = 0;    // length of the flat run (1 for InterSamplePeak point events)
};

struct ClippingParameters {
    double        ceilingDbfs         = 0.0;   // nominal; actual ceiling is source-format-derived, see deriveCeiling()
    std::uint32_t  minRunSamples       = 3;     // M11: "the standard convention"; a genuine trade-off, kept configurable
    double         nearClipDbfs        = -0.1;
    std::uint32_t  nearClipMinRun      = 3;
    double         flatnessToleranceDb = 0.0;   // >0 widens the "at ceiling" test to catch soft-clipped plateaus
    std::uint32_t  mergeGapSamples     = 32;
    std::uint32_t  ispOversampling     = 4;      // 4, 8 or 16 — passed straight to TruePeakMeter
    bool           detectInterSamplePeaks = true;
    std::uint32_t  maxStoredEvents     = 10000;  // BoundedTopN capacity; counts stay exact regardless
};

struct ClippingResult {
    std::vector<ClipEvent> events;  // capped at maxStoredEvents, keeping the worst by overshoot
    std::uint64_t totalClippedSamples = 0;  // Digital + OverFullScale sample-frames, summed over channels
    std::array<std::uint32_t, kClipKindCount> eventCount{};  // uncapped, indexed by ClipKind
    double clippedFraction    = 0.0;  // totalClippedSamples / (frameCount * channelCount)
    double maxOvershootDb     = 0.0;  // max dB above 0 dBFS/0 dBTP seen (OverFullScale/InterSamplePeak only)
    double flatTopRatio       = 0.0;  // M11's limiting heuristic — see limiting_heuristic.hpp
    double meanPlateauLength  = 0.0;
    bool   heavyLimitingLikely = false;
    std::uint32_t containerBitDepth = 0;

    // Per-bin clipping intensity, aligned to the waveform pyramid's level-0 bin size
    // (waveform::kBaseBinFrames — see densityBinFrames) so the timeline can shade regions by
    // clipping density even when individual markers would be a solid wall (M11 "Output"). Value
    // per bin is the fraction of that bin's sample-frames (summed over channels) that were Digital
    // or OverFullScale; NearClip/InterSamplePeak are not weighted in (they're advisory, not damage).
    std::vector<float>  densitySeries;
    std::uint32_t        densityBinFrames = 0;

    ClippingParameters parametersUsed;

    // Stable JSON serialisation, matching SilenceResult::toJson()'s convention.
    [[nodiscard]] std::string toJson() const;
};

struct ClippingConfig {
    // Source integer container bit depth (e.g. 16, 24), or 0 for float sources — see
    // decoder::StreamInfo::bitDepth (M02), same convention as StatisticsConfig::containerBitDepth.
    std::uint32_t       containerBitDepth = 0;
    ClippingParameters  parameters;
};

// Derives the ceiling (linear amplitude) that Digital clipping is measured against: for an
// n-bit integer container, M02 divides by 2^(n-1), so the true ceiling is
// (2^(n-1) - 1) / 2^(n-1), not 1.0. Returns 1.0 for float sources (containerBitDepth == 0) —
// callers must not run Digital detection against that value; use OverFullScale instead (see
// ClipDetectorAnalyzer::process()).
[[nodiscard]] double deriveCeilingLinear(std::uint32_t containerBitDepth) noexcept;

// Non-owning-target design, mirroring StatisticsAnalyzer/LoudnessAnalyzer: the caller owns a
// plain-data ClippingResult and this analyser only ever writes into it.
class ClipDetectorAnalyzer final : public Analyzer {
public:
    explicit ClipDetectorAnalyzer(ClippingResult& result, ClippingConfig config = {});
    ~ClipDetectorAnalyzer() override;

    [[nodiscard]] std::string_view id() const noexcept override { return "analysis.clipping"; }
    [[nodiscard]] std::uint32_t    version() const noexcept override { return 1; }

    Result<void>           begin(const AudioSpec& spec) override;
    Result<void>           process(const ChunkView& chunk) override;
    Result<AnalysisResult> finish() override;

private:
    struct RunState;      // per (channel, kind) run-length tracker, defined in the .cpp
    struct DensityState;  // per-bin clipping accumulator, defined in the .cpp

    ClippingResult* m_result;
    ClippingConfig  m_config;
    SampleRate      m_sampleRate = 0;
    ChannelIndex    m_channels   = 0;
    FrameIndex      m_frameCursor = 0;
    double          m_ceilingLinear = 1.0;
    bool            m_isFloatSource = false;
    double          m_digitalOrOverFullScaleThresholdLinear = 1.0;
    double          m_nearClipThresholdLinear                = 1.0;

    std::vector<RunState> m_digitalOrOverFullScale;  // one per channel
    std::vector<RunState> m_nearClip;                // one per channel

    std::unique_ptr<loudness::TruePeakMeter> m_truePeak;

    std::unique_ptr<DensityState> m_density;

    std::unique_ptr<LimitingHeuristicAccumulator> m_limiting;
    std::vector<double>                            m_channelPeakLinear;  // exact per-channel |x| max, for flat-top ratio

    using EventHeap = util::BoundedTopN<ClipEvent, double (*)(const ClipEvent&)>;
    std::unique_ptr<EventHeap> m_eventHeap;

    void      emitEvent(ClipEvent event);
    [[nodiscard]] ClipEvent makeEvent(ClipKind kind, ChannelIndex channel, FrameIndex begin, FrameIndex end,
                                        double peakLinear, std::uint32_t sampleCount) const;
};

// Factory, mirroring makeStatisticsAnalyzer/makeLoudnessAnalyzer.
[[nodiscard]] std::unique_ptr<Analyzer> makeClipDetectorAnalyzer(ClippingResult& result, ClippingConfig config = {});

}  // namespace aud::clipping

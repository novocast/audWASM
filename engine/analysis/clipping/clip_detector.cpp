#include "clip_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <utility>

#include "../../waveform/waveform_bin.hpp"
#include "limiting_heuristic.hpp"

namespace aud::clipping {

namespace {
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double linearToDbfs(double linear) noexcept { return linear > 0.0 ? 20.0 * std::log10(linear) : kNegInf; }

std::string jsonNumber(double v) {
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return v > 0 ? "1e999" : "-1e999";  // JSON has no Infinity; sentinel large magnitude
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

const char* kindName(ClipKind kind) {
    switch (kind) {
        case ClipKind::Digital:         return "digital";
        case ClipKind::OverFullScale:    return "overFullScale";
        case ClipKind::NearClip:         return "nearClip";
        case ClipKind::InterSamplePeak:  return "interSamplePeak";
    }
    return "unknown";
}

// Severity key for BoundedTopN: how far past the *relevant* ceiling an event's peak sits, in dB.
// For Digital (which by construction can never exceed the source ceiling — that ceiling *is* the
// representable maximum) this is always ~0; longer runs at the ceiling are not "worse" by this
// metric, only by sampleCount, so ties break arbitrarily among them, which is acceptable (M11 only
// asks that the *worst by overshoot* survive capping, and Digital events have none to speak of).
double overshootKey(const ClipEvent& event) {
    switch (event.kind) {
        case ClipKind::Digital:
            return event.peakDbfs;  // effectively constant; longer runs still separated by count via peakDbfs ties
        case ClipKind::OverFullScale:
        case ClipKind::InterSamplePeak:
            return event.peakDbfs;  // both already 0-referenced (dBFS/dBTP), larger = more severe
        case ClipKind::NearClip:
            return event.peakDbfs;
    }
    return event.peakDbfs;
}

}  // namespace

double deriveCeilingLinear(std::uint32_t containerBitDepth) noexcept {
    if (containerBitDepth == 0) return 1.0;  // float source: no hard ceiling, see OverFullScale
    const double halfRange = static_cast<double>(std::uint64_t{1} << (containerBitDepth - 1));
    return (halfRange - 1.0) / halfRange;
}

// One (channel, kind) run-length tracker. Streams samples in absolute-frame order across chunk
// boundaries, merging runs separated by less than mergeGapSamples and discarding anything shorter
// than minRunSamples — same shape as M10's silence run-length pipeline, but at sample (not window)
// granularity and driven sample-by-sample rather than over a precomputed boolean vector, since
// there is no equivalent pre-reduced series available at this resolution.
struct ClipDetectorAnalyzer::RunState {
    template <class EmitFn>
    void feed(FrameIndex frame, double magnitude, bool meets, std::uint32_t minRunSamples,
              std::uint32_t mergeGapSamples, EmitFn&& emit) {
        if (meets) {
            if (!m_active) {
                m_active = true;
                m_start  = frame;
                m_peak   = magnitude;
                m_count  = 1;
            } else {
                m_peak = std::max(m_peak, magnitude);
                ++m_count;
            }
        } else if (m_active) {
            closeActive(frame, minRunSamples, mergeGapSamples, emit);
        }
    }

    template <class EmitFn>
    void finish(FrameIndex endFrame, std::uint32_t minRunSamples, std::uint32_t mergeGapSamples, EmitFn&& emit) {
        if (m_active) closeActive(endFrame, minRunSamples, mergeGapSamples, emit);
        flushPendingRun(minRunSamples, emit);
    }

private:
    template <class EmitFn>
    void closeActive(FrameIndex endFrame, std::uint32_t minRunSamples, std::uint32_t mergeGapSamples, EmitFn&& emit) {
        if (m_hasPending) {
            const FrameIndex gap = m_start - m_pendingEnd;
            if (gap <= static_cast<FrameIndex>(mergeGapSamples)) {
                m_pendingEnd   = endFrame;
                m_pendingPeak  = std::max(m_pendingPeak, m_peak);
                m_pendingCount = static_cast<std::uint32_t>(m_pendingEnd - m_pendingStart);
                m_active = false;
                m_count  = 0;
                return;
            }
            flushPendingRun(minRunSamples, emit);
        }
        m_pendingStart = m_start;
        m_pendingEnd   = endFrame;
        m_pendingPeak  = m_peak;
        m_pendingCount = m_count;
        m_hasPending   = true;
        m_active       = false;
        m_count        = 0;
    }

    template <class EmitFn>
    void flushPendingRun(std::uint32_t minRunSamples, EmitFn&& emit) {
        if (!m_hasPending) return;
        if (m_pendingCount >= minRunSamples) {
            emit(m_pendingStart, m_pendingEnd, m_pendingPeak, m_pendingCount);
        }
        m_hasPending = false;
    }

    bool          m_active = false;
    FrameIndex    m_start  = 0;
    double        m_peak   = 0.0;
    std::uint32_t m_count  = 0;

    bool          m_hasPending   = false;
    FrameIndex    m_pendingStart = 0;
    FrameIndex    m_pendingEnd   = 0;
    double        m_pendingPeak  = 0.0;
    std::uint32_t m_pendingCount = 0;
};

// Per-bin clipping-intensity accumulator, aligned to waveform::kBaseBinFrames (M11 "Output":
// "maintain a per-bin clipping density series aligned with the waveform pyramid"). Grows lazily —
// frameCount may be unknown up front for progressively-decoded sources (AudioSpec::frameCount can
// be kNoFrame), same as every other streaming accumulator in this codebase.
struct ClipDetectorAnalyzer::DensityState {
    std::uint32_t                binFrames = waveform::kBaseBinFrames;
    ChannelIndex                 channels  = 0;
    std::vector<std::uint32_t>   clippedPerBin;

    void begin(ChannelIndex ch) {
        channels = ch;
        clippedPerBin.clear();
    }

    void markClipped(FrameIndex frame) {
        if (binFrames == 0 || frame < 0) return;
        const auto bin = static_cast<std::size_t>(frame) / binFrames;
        if (bin >= clippedPerBin.size()) clippedPerBin.resize(bin + 1, 0);
        ++clippedPerBin[bin];
    }

    [[nodiscard]] std::vector<float> toSeries() const {
        std::vector<float> out(clippedPerBin.size());
        const double denom = static_cast<double>(binFrames) * static_cast<double>(std::max<ChannelIndex>(channels, 1));
        for (std::size_t i = 0; i < clippedPerBin.size(); ++i) {
            out[i] = denom > 0.0 ? static_cast<float>(std::min(1.0, static_cast<double>(clippedPerBin[i]) / denom)) : 0.0f;
        }
        return out;
    }
};

ClipDetectorAnalyzer::ClipDetectorAnalyzer(ClippingResult& result, ClippingConfig config)
    : m_result(&result), m_config(std::move(config)) {}

ClipDetectorAnalyzer::~ClipDetectorAnalyzer() = default;

Result<void> ClipDetectorAnalyzer::begin(const AudioSpec& spec) {
    if (spec.sampleRate == 0 || spec.channels == 0) {
        return Error{ErrorCode::InvalidArgument, "analysis.clipping",
                      "ClipDetectorAnalyzer requires a non-zero sample rate and channel count"};
    }

    m_sampleRate  = spec.sampleRate;
    m_channels    = spec.channels;
    m_frameCursor = 0;

    m_isFloatSource = m_config.containerBitDepth == 0;
    m_ceilingLinear = deriveCeilingLinear(m_config.containerBitDepth);

    // flatnessToleranceDb widens the "at ceiling" test so soft-clipped plateaus a fraction of a dB
    // below the hard ceiling still register as a flat run (M11: ">0 catches soft-clipped
    // plateaus"). 0 (the default) reduces this to an exact >= test.
    const double toleranceLinear =
        m_config.parameters.flatnessToleranceDb > 0.0
            ? m_ceilingLinear * (1.0 - std::pow(10.0, -m_config.parameters.flatnessToleranceDb / 20.0))
            : 0.0;
    m_digitalOrOverFullScaleThresholdLinear = std::max(0.0, m_ceilingLinear - toleranceLinear);
    m_nearClipThresholdLinear               = std::pow(10.0, m_config.parameters.nearClipDbfs / 20.0);

    m_digitalOrOverFullScale.assign(m_channels, RunState{});
    m_nearClip.assign(m_channels, RunState{});

    m_density = std::make_unique<DensityState>();
    m_density->begin(m_channels);

    m_eventHeap = std::make_unique<EventHeap>(m_config.parameters.maxStoredEvents, &overshootKey);

    m_limiting = std::make_unique<LimitingHeuristicAccumulator>();
    m_limiting->begin(m_channels);
    m_channelPeakLinear.assign(m_channels, 0.0);

    if (m_config.parameters.detectInterSamplePeaks) {
        m_truePeak = std::make_unique<loudness::TruePeakMeter>(m_sampleRate, m_channels,
                                                                  m_config.parameters.ispOversampling);
        m_truePeak->enableIspEventCapture(/*thresholdDbtp=*/0.0, /*maxEvents=*/0);
    }

    *m_result = ClippingResult{};
    m_result->parametersUsed    = m_config.parameters;
    m_result->containerBitDepth = m_config.containerBitDepth;

    return {};
}

void ClipDetectorAnalyzer::emitEvent(ClipEvent event) {
    ++m_result->eventCount[static_cast<std::size_t>(event.kind)];
    if (event.kind == ClipKind::Digital || event.kind == ClipKind::OverFullScale) {
        m_result->totalClippedSamples += event.sampleCount;
        if (event.kind == ClipKind::OverFullScale) {
            m_result->maxOvershootDb = std::max(m_result->maxOvershootDb, event.peakDbfs);
        }
    }
    m_eventHeap->push(std::move(event));
}

Result<void> ClipDetectorAnalyzer::process(const ChunkView& chunk) {
    const std::size_t frameCount = chunk.frameCount();
    if (frameCount == 0) return {};

    for (ChannelIndex ch = 0; ch < m_channels && ch < chunk.channels.size(); ++ch) {
        std::span<const Sample> samples = chunk.channels[ch];
        RunState&               digitalRun = m_digitalOrOverFullScale[ch];
        RunState&               nearRun    = m_nearClip[ch];
        const ClipKind          hardKind   = m_isFloatSource ? ClipKind::OverFullScale : ClipKind::Digital;

        for (std::size_t i = 0; i < samples.size(); ++i) {
            const FrameIndex frame     = m_frameCursor + static_cast<FrameIndex>(i);
            const double     magnitude = static_cast<double>(std::fabs(samples[i]));

            m_channelPeakLinear[ch] = std::max(m_channelPeakLinear[ch], magnitude);

            const bool meetsHard = magnitude >= m_digitalOrOverFullScaleThresholdLinear;
            const bool meetsNear = magnitude >= m_nearClipThresholdLinear;

            if (meetsHard) m_density->markClipped(frame);

            digitalRun.feed(frame, magnitude, meetsHard, m_config.parameters.minRunSamples,
                             m_config.parameters.mergeGapSamples,
                             [&](FrameIndex begin, FrameIndex end, double peak, std::uint32_t count) {
                                 emitEvent(makeEvent(hardKind, ch, begin, end, peak, count));
                             });
            nearRun.feed(frame, magnitude, meetsNear, m_config.parameters.nearClipMinRun,
                         m_config.parameters.mergeGapSamples,
                         [&](FrameIndex begin, FrameIndex end, double peak, std::uint32_t count) {
                             emitEvent(makeEvent(ClipKind::NearClip, ch, begin, end, peak, count));
                         });
        }

        m_limiting->process(ch, samples);
    }

    if (m_truePeak) m_truePeak->process(chunk.channels);

    m_frameCursor += static_cast<FrameIndex>(frameCount);
    return {};
}

ClipEvent ClipDetectorAnalyzer::makeEvent(ClipKind kind, ChannelIndex channel, FrameIndex begin, FrameIndex end,
                                            double peakLinear, std::uint32_t sampleCount) const {
    ClipEvent event;
    event.range        = FrameRange{begin, end};
    event.startSeconds = m_sampleRate == 0 ? 0.0 : static_cast<double>(begin) / static_cast<double>(m_sampleRate);
    event.endSeconds   = m_sampleRate == 0 ? 0.0 : static_cast<double>(end) / static_cast<double>(m_sampleRate);
    event.channel      = channel;
    event.kind         = kind;
    event.peakValue    = peakLinear;
    event.peakDbfs     = linearToDbfs(peakLinear);
    event.sampleCount  = sampleCount;
    return event;
}

Result<AnalysisResult> ClipDetectorAnalyzer::finish() {
    // Flush every channel's still-open runs (a clipped run touching end-of-file never sees a
    // "not meeting threshold" sample to trigger closeActive()).
    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        const ClipKind hardKind = m_isFloatSource ? ClipKind::OverFullScale : ClipKind::Digital;
        m_digitalOrOverFullScale[ch].finish(m_frameCursor, m_config.parameters.minRunSamples,
                                             m_config.parameters.mergeGapSamples,
                                             [&](FrameIndex begin, FrameIndex end, double peak, std::uint32_t count) {
                                                 emitEvent(makeEvent(hardKind, ch, begin, end, peak, count));
                                             });
        m_nearClip[ch].finish(m_frameCursor, m_config.parameters.nearClipMinRun, m_config.parameters.mergeGapSamples,
                               [&](FrameIndex begin, FrameIndex end, double peak, std::uint32_t count) {
                                   emitEvent(makeEvent(ClipKind::NearClip, ch, begin, end, peak, count));
                               });
    }

    if (m_truePeak) {
        m_truePeak->finish();
        for (const loudness::IspEvent& isp : m_truePeak->ispEvents()) {
            ClipEvent event;
            event.range        = FrameRange{isp.frame, isp.frame + 1};
            event.startSeconds = m_sampleRate == 0 ? 0.0 : static_cast<double>(isp.frame) / static_cast<double>(m_sampleRate);
            event.endSeconds   = event.startSeconds;
            event.channel      = isp.channel;
            event.kind         = ClipKind::InterSamplePeak;
            event.peakValue    = std::pow(10.0, isp.truePeakDbtp / 20.0);
            event.peakDbfs     = isp.truePeakDbtp;  // dBTP, per ClipEvent::peakDbfs's doc comment
            event.sampleCount  = 1;
            emitEvent(std::move(event));
        }
        // ispEventCountTotal() is the exact count regardless of any cap TruePeakMeter itself was
        // given (none here — capping is delegated entirely to m_eventHeap below).
        m_result->eventCount[static_cast<std::size_t>(ClipKind::InterSamplePeak)] =
            static_cast<std::uint32_t>(m_truePeak->ispEventCountTotal());
        m_result->maxOvershootDb = std::max(m_result->maxOvershootDb, m_truePeak->truePeakDbtpOverall());
    }

    const LimitingHeuristicResult limiting = m_limiting->finish(m_channelPeakLinear);
    m_result->flatTopRatio        = limiting.flatTopRatio;
    m_result->meanPlateauLength   = limiting.meanPlateauLength;
    m_result->heavyLimitingLikely = limiting.heavyLimitingLikely;

    m_result->events = m_eventHeap->extractSorted();

    const std::uint64_t totalSampleFrames =
        static_cast<std::uint64_t>(m_frameCursor) * static_cast<std::uint64_t>(m_channels);
    m_result->clippedFraction =
        totalSampleFrames > 0 ? static_cast<double>(m_result->totalClippedSamples) / static_cast<double>(totalSampleFrames)
                              : 0.0;

    m_result->densitySeries    = m_density->toSeries();
    m_result->densityBinFrames = m_density->binFrames;

    return AnalysisResult{};
}

std::unique_ptr<Analyzer> makeClipDetectorAnalyzer(ClippingResult& result, ClippingConfig config) {
    return std::make_unique<ClipDetectorAnalyzer>(result, std::move(config));
}

std::string ClippingResult::toJson() const {
    std::ostringstream out;
    out << "{\"schemaVersion\":\"1.0.0\",";
    out << "\"totalClippedSamples\":" << totalClippedSamples << ",";
    out << "\"clippedFraction\":" << jsonNumber(clippedFraction) << ",";
    out << "\"maxOvershootDb\":" << jsonNumber(maxOvershootDb) << ",";
    out << "\"flatTopRatio\":" << jsonNumber(flatTopRatio) << ",";
    out << "\"meanPlateauLength\":" << jsonNumber(meanPlateauLength) << ",";
    out << "\"heavyLimitingLikely\":" << (heavyLimitingLikely ? "true" : "false") << ",";
    out << "\"containerBitDepth\":" << containerBitDepth << ",";

    out << "\"eventCount\":{";
    for (std::size_t k = 0; k < kClipKindCount; ++k) {
        if (k > 0) out << ",";
        out << "\"" << kindName(static_cast<ClipKind>(k)) << "\":" << eventCount[k];
    }
    out << "},";

    out << "\"parametersUsed\":{";
    out << "\"ceilingDbfs\":" << jsonNumber(parametersUsed.ceilingDbfs) << ",";
    out << "\"minRunSamples\":" << parametersUsed.minRunSamples << ",";
    out << "\"nearClipDbfs\":" << jsonNumber(parametersUsed.nearClipDbfs) << ",";
    out << "\"nearClipMinRun\":" << parametersUsed.nearClipMinRun << ",";
    out << "\"flatnessToleranceDb\":" << jsonNumber(parametersUsed.flatnessToleranceDb) << ",";
    out << "\"mergeGapSamples\":" << parametersUsed.mergeGapSamples << ",";
    out << "\"ispOversampling\":" << parametersUsed.ispOversampling << ",";
    out << "\"detectInterSamplePeaks\":" << (parametersUsed.detectInterSamplePeaks ? "true" : "false") << ",";
    out << "\"maxStoredEvents\":" << parametersUsed.maxStoredEvents;
    out << "},";

    out << "\"events\":[";
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i > 0) out << ",";
        const auto& e = events[i];
        out << "{";
        out << "\"beginFrame\":" << e.range.begin << ",";
        out << "\"endFrame\":" << e.range.end << ",";
        out << "\"startSeconds\":" << jsonNumber(e.startSeconds) << ",";
        out << "\"endSeconds\":" << jsonNumber(e.endSeconds) << ",";
        out << "\"channel\":" << e.channel << ",";
        out << "\"kind\":\"" << kindName(e.kind) << "\",";
        out << "\"peakValue\":" << jsonNumber(e.peakValue) << ",";
        out << "\"peakDbfs\":" << jsonNumber(e.peakDbfs) << ",";
        out << "\"sampleCount\":" << e.sampleCount;
        out << "}";
    }
    out << "]";

    out << "}";
    return out.str();
}

}  // namespace aud::clipping

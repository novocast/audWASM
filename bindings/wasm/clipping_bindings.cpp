// Embind surface for M11's clipping engine. Same non-owning audioBufferHandle handoff and
// processAvailableChunks()/finish() polling contract as Waveform/Loudness/Statistics — driven
// directly against an existing aud::AudioBuffer, bypassing the Analyzer registry (M20 isn't built
// yet). `containerBitDepth` is passed through the same way Statistics takes it (0 for float
// sources) — see decoder::StreamInfo::bitDepth (M02).
//
// Events are handed back as a val::array of small val::objects (like Silence's regions, not
// {ptr,count}) since the list is capped at parametersUsed.maxStoredEvents (default 10000) and is
// therefore small relative to the RMS/histogram series elsewhere; eventCount/totalOffered convey
// the exact uncapped totals per M11's "never silently truncate" decision. The density series is a
// {ptr,length} heap view per M01's convention, since it can be as long as the waveform pyramid's
// level-0 bin count.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../../engine/analysis/clipping/clip_detector.hpp"
#include "../../engine/util/audio_buffer.hpp"

using emscripten::val;

namespace bindings {

namespace {

val errorToVal(const aud::Error& error) {
    val out = val::object();
    out.set("ok", false);
    out.set("code", std::string(aud::toString(error.code)));
    out.set("detail", error.detail);
    return out;
}

val okVal() {
    val out = val::object();
    out.set("ok", true);
    return out;
}

double ptrOf(const void* data) { return static_cast<double>(reinterpret_cast<std::uintptr_t>(data)); }

val eventToVal(const aud::clipping::ClipEvent& e) {
    val out = val::object();
    out.set("beginFrame", static_cast<double>(e.range.begin));
    out.set("endFrame", static_cast<double>(e.range.end));
    out.set("startSeconds", e.startSeconds);
    out.set("endSeconds", e.endSeconds);
    out.set("channel", e.channel);
    out.set("kind", static_cast<std::uint32_t>(e.kind));
    out.set("peakValue", e.peakValue);
    out.set("peakDbfs", e.peakDbfs);
    out.set("sampleCount", e.sampleCount);
    return out;
}

}  // namespace

class ClippingHandle {
public:
    // `containerBitDepth` is the source integer container's depth (0 for float sources) — see
    // decoder::StreamInfo::bitDepth (M02).
    static std::unique_ptr<ClippingHandle> create(std::uintptr_t audioBufferHandle, std::uint32_t containerBitDepth) {
        const auto* buffer = reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle);
        if (buffer == nullptr) {
            return nullptr;
        }

        aud::clipping::ClippingConfig config;
        config.containerBitDepth = containerBitDepth;

        auto handle = std::unique_ptr<ClippingHandle>(new ClippingHandle(buffer, config));
        const aud::AudioSpec spec{buffer->sampleRate(), buffer->channelCount(), buffer->frameCount()};
        if (!handle->m_analyzer->begin(spec).has_value()) {
            return nullptr;
        }
        return handle;
    }

    // `params`: [minRunSamples, nearClipMinRun, mergeGapSamples, ispOversampling, maxStoredEvents]
    // and the double params [nearClipDbfs, flatnessToleranceDb]; 0/default-constructed
    // ClippingParameters otherwise. Kept as individual scalar setters (rather than a JS object) to
    // match every other *_bindings.cpp in this codebase — Embind's automatic struct marshalling
    // isn't used anywhere here.
    void configure(std::uint32_t minRunSamples, double nearClipDbfs, std::uint32_t nearClipMinRun,
                   double flatnessToleranceDb, std::uint32_t mergeGapSamples, std::uint32_t ispOversampling,
                   bool detectInterSamplePeaks, std::uint32_t maxStoredEvents) {
        auto& p                    = m_config.parameters;
        p.minRunSamples             = minRunSamples;
        p.nearClipDbfs              = nearClipDbfs;
        p.nearClipMinRun            = nearClipMinRun;
        p.flatnessToleranceDb       = flatnessToleranceDb;
        p.mergeGapSamples           = mergeGapSamples;
        p.ispOversampling           = ispOversampling == 0 ? 4 : ispOversampling;
        p.detectInterSamplePeaks    = detectInterSamplePeaks;
        p.maxStoredEvents           = maxStoredEvents;

        // configure() must be called before the first processAvailableChunks() — re-begin() so the
        // freshly-set parameters actually take effect (begin() is where thresholds are derived).
        const aud::AudioSpec spec{m_buffer->sampleRate(), m_buffer->channelCount(), m_buffer->frameCount()};
        m_analyzer = aud::clipping::makeClipDetectorAnalyzer(m_result, m_config);
        m_analyzer->begin(spec);
        m_nextChunk   = 0;
    }

    // Feeds every AudioBuffer chunk appended since the last call — safe to call repeatedly
    // alongside progressive decode, same polling contract as Waveform/Loudness/Statistics.
    val processAvailableChunks() {
        const std::size_t       chunkCount = m_buffer->chunkCount();
        const aud::ChannelIndex channels   = m_buffer->channelCount();

        while (m_nextChunk < chunkCount) {
            std::vector<std::span<const aud::Sample>> planar(channels);
            for (aud::ChannelIndex ch = 0; ch < channels; ++ch) {
                planar[ch] = m_buffer->chunk(ch, m_nextChunk);
            }
            const aud::ChunkView view{std::span<const std::span<const aud::Sample>>(planar), 0};
            auto                 result = m_analyzer->process(view);
            if (!result.has_value()) {
                return errorToVal(result.error());
            }
            ++m_nextChunk;
        }
        return okVal();
    }

    // Runs the run-length flush, ISP pass and limiting heuristic; call once after decode is
    // complete.
    val finish() {
        auto result = m_analyzer->finish();
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        return resultToVal();
    }

private:
    ClippingHandle(const aud::AudioBuffer* buffer, const aud::clipping::ClippingConfig& config)
        : m_buffer(buffer), m_config(config), m_analyzer(aud::clipping::makeClipDetectorAnalyzer(m_result, config)) {}

    val resultToVal() const {
        val out = val::object();
        out.set("ok", true);
        out.set("totalClippedSamples", static_cast<double>(m_result.totalClippedSamples));
        out.set("clippedFraction", m_result.clippedFraction);
        out.set("maxOvershootDb", m_result.maxOvershootDb);
        out.set("flatTopRatio", m_result.flatTopRatio);
        out.set("meanPlateauLength", m_result.meanPlateauLength);
        out.set("heavyLimitingLikely", m_result.heavyLimitingLikely);
        out.set("containerBitDepth", m_result.containerBitDepth);
        out.set("reportJson", m_result.toJson());

        val eventCount = val::array();
        for (std::size_t k = 0; k < aud::clipping::kClipKindCount; ++k) {
            eventCount.set(k, m_result.eventCount[k]);
        }
        out.set("eventCount", eventCount);

        val events = val::array();
        for (std::size_t i = 0; i < m_result.events.size(); ++i) {
            events.set(i, eventToVal(m_result.events[i]));
        }
        out.set("events", events);
        out.set("eventsCapped", m_result.events.size() <
                                     m_result.eventCount[0] + m_result.eventCount[1] + m_result.eventCount[2] +
                                         m_result.eventCount[3]);

        out.set("densitySeriesPtr", ptrOf(m_result.densitySeries.data()));
        out.set("densitySeriesCount", static_cast<std::uint32_t>(m_result.densitySeries.size()));
        out.set("densityBinFrames", m_result.densityBinFrames);

        return out;
    }

    const aud::AudioBuffer*             m_buffer;
    aud::clipping::ClippingConfig       m_config;
    aud::clipping::ClippingResult       m_result;
    std::unique_ptr<aud::Analyzer>      m_analyzer;
    std::size_t                         m_nextChunk = 0;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_clipping) {
    emscripten::class_<bindings::ClippingHandle>("Clipping")
        .class_function("create", &bindings::ClippingHandle::create)
        .function("configure", &bindings::ClippingHandle::configure)
        .function("processAvailableChunks", &bindings::ClippingHandle::processAvailableChunks)
        .function("finish", &bindings::ClippingHandle::finish);
}

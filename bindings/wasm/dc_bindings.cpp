// Embind surface for M12's DC offset analyser. Same non-owning audioBufferHandle handoff and
// processAvailableChunks()/finish() polling contract as Waveform/Loudness/Statistics/Clipping —
// driven directly against an existing aud::AudioBuffer, bypassing the Analyzer registry (M20 isn't
// built yet).
//
// The windowed series is handed back as a {ptr,count} heap view per M01's convention (can be as
// long as the file is seconds), matching Statistics's rmsSeries; per-channel results and
// stepLocations are small, so they go back as val::objects/val::arrays like Silence's regions.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../../engine/analysis/dc/dc_analyzer.hpp"
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

val channelToVal(const aud::dc::ChannelDcResult& c) {
    val out = val::object();
    out.set("offsetLinear", c.offsetLinear);
    out.set("offsetDbfs", c.offsetDbfs);
    out.set("offsetPercent", c.offsetPercent);
    out.set("pattern", static_cast<std::uint32_t>(c.pattern));
    out.set("minWindowOffset", c.minWindowOffset);
    out.set("maxWindowOffset", c.maxWindowOffset);
    out.set("headroomLostDb", c.headroomLostDb);
    out.set("peakAfterCorrectionDbfs", c.peakAfterCorrectionDbfs);
    out.set("recommendedHighpassHz", c.recommendedHighpassHz);

    val steps = val::array();
    for (std::size_t i = 0; i < c.stepLocations.size(); ++i) {
        steps.set(i, static_cast<double>(c.stepLocations[i]));
    }
    out.set("stepLocations", steps);

    return out;
}

}  // namespace

class DcHandle {
public:
    static std::unique_ptr<DcHandle> create(std::uintptr_t audioBufferHandle) {
        const auto* buffer = reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle);
        if (buffer == nullptr) {
            return nullptr;
        }

        auto handle = std::unique_ptr<DcHandle>(new DcHandle(buffer));
        const aud::AudioSpec spec{buffer->sampleRate(), buffer->channelCount(), buffer->frameCount()};
        if (!handle->m_analyzer->begin(spec).has_value()) {
            return nullptr;
        }
        return handle;
    }

    // Re-begins with a new significance threshold — must be called (if at all) before the first
    // processAvailableChunks(), same convention as Clipping::configure().
    void configure(double significanceThresholdDbfs) {
        m_config.significanceThresholdDbfs = significanceThresholdDbfs;
        m_analyzer                          = aud::dc::makeDcAnalyzer(m_result, m_config);
        const aud::AudioSpec spec{m_buffer->sampleRate(), m_buffer->channelCount(), m_buffer->frameCount()};
        m_analyzer->begin(spec);
        m_nextChunk = 0;
    }

    // Feeds every AudioBuffer chunk appended since the last call — safe to call repeatedly
    // alongside progressive decode, same polling contract as Waveform/Loudness/Statistics/Clipping.
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

    val finish() {
        auto result = m_analyzer->finish();
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        return resultToVal();
    }

private:
    explicit DcHandle(const aud::AudioBuffer* buffer)
        : m_buffer(buffer), m_analyzer(aud::dc::makeDcAnalyzer(m_result, m_config)) {}

    val resultToVal() const {
        val out = val::object();
        out.set("ok", true);
        out.set("significanceThresholdDbfs", m_result.significanceThresholdDbfs);
        out.set("anySignificant", m_result.anySignificant);
        out.set("windowSeconds", m_result.windowSeconds);
        out.set("reportJson", m_result.toJson());

        val channels = val::array();
        for (std::size_t i = 0; i < m_result.channels.size(); ++i) {
            channels.set(i, channelToVal(m_result.channels[i]));
        }
        out.set("channels", channels);

        out.set("windowSeriesPtr", ptrOf(m_result.windowSeries.data()));
        out.set("windowSeriesCount", static_cast<std::uint32_t>(m_result.windowSeries.size()));
        out.set("windowSeriesChannelCount", m_result.windowSeriesChannelCount);

        return out;
    }

    const aud::AudioBuffer*        m_buffer;
    aud::dc::DcConfig               m_config;
    aud::dc::DcOffsetResult         m_result;
    std::unique_ptr<aud::Analyzer> m_analyzer;
    std::size_t                    m_nextChunk = 0;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_dc) {
    emscripten::class_<bindings::DcHandle>("Dc")
        .class_function("create", &bindings::DcHandle::create)
        .function("configure", &bindings::DcHandle::configure)
        .function("processAvailableChunks", &bindings::DcHandle::processAvailableChunks)
        .function("finish", &bindings::DcHandle::finish);
}

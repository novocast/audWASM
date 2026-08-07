// Embind surface for M09's statistics engine. Same non-owning audioBufferHandle handoff and
// processAvailableChunks()/finish() polling contract as Waveform/Loudness (waveform_bindings.cpp,
// loudness_bindings.cpp) — driven directly against an existing aud::AudioBuffer, bypassing the
// Analyzer registry (M20 isn't built yet).
//
// Bulk data (per-channel histograms, the interleaved RMS series, the correlation series) is handed
// back as {ptr, length} heap views per M01's binding convention; finish() also returns the full
// JSON report string (StatisticsResult::toJson(), docs/report-schema.json) since M09 decided that
// JSON shape is itself a stable public artifact worth exposing directly rather than only via the
// CLI.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../../engine/analysis/statistics/statistics_analyzer.hpp"
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

}  // namespace

class StatisticsHandle {
public:
    // `containerBitDepth` is the source integer container's depth (0 for float sources) — see
    // decoder::StreamInfo::bitDepth (M02).
    static std::unique_ptr<StatisticsHandle> create(std::uintptr_t audioBufferHandle,
                                                       std::uint32_t containerBitDepth) {
        const auto* buffer = reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle);
        if (buffer == nullptr) {
            return nullptr;
        }

        aud::statistics::StatisticsConfig config;
        config.containerBitDepth = containerBitDepth;

        auto handle = std::unique_ptr<StatisticsHandle>(new StatisticsHandle(buffer, config));
        const aud::AudioSpec spec{buffer->sampleRate(), buffer->channelCount(), buffer->frameCount()};
        if (!handle->m_analyzer->begin(spec).has_value()) {
            return nullptr;
        }
        return handle;
    }

    // Feeds every AudioBuffer chunk appended since the last call — safe to call repeatedly
    // alongside progressive decode, same polling contract as Waveform/Loudness.
    val processAvailableChunks() {
        const std::size_t       chunkCount = m_buffer->chunkCount();
        const aud::ChannelIndex channels   = m_buffer->channelCount();

        while (m_nextChunk < chunkCount) {
            std::vector<std::span<const aud::Sample>> planar(channels);
            for (aud::ChannelIndex ch = 0; ch < channels; ++ch) {
                planar[ch] = m_buffer->chunk(ch, m_nextChunk);
            }
            const aud::ChunkView view{std::span<const std::span<const aud::Sample>>(planar), m_frameCursor};
            auto                 result = m_analyzer->process(view);
            if (!result.has_value()) {
                return errorToVal(result.error());
            }
            m_frameCursor += static_cast<aud::FrameIndex>(view.frameCount());
            ++m_nextChunk;
        }
        return okVal();
    }

    // Runs the finish() pass (bit depth, dynamic range, stereo correlation); call once after
    // decode is complete.
    val finish() {
        auto result = m_analyzer->finish();
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        return resultToVal();
    }

private:
    StatisticsHandle(const aud::AudioBuffer* buffer, const aud::statistics::StatisticsConfig& config)
        : m_buffer(buffer), m_analyzer(aud::statistics::makeStatisticsAnalyzer(m_result, config)) {}

    val resultToVal() const {
        val out = val::object();
        out.set("ok", true);
        out.set("sampleRate", m_result.sampleRate);
        out.set("channelCount", m_result.channelCount);
        out.set("frameCount", static_cast<double>(m_result.frameCount));
        out.set("crestFactorDb", m_result.crestFactorDb);
        out.set("dynamicRangeDr", m_result.dynamicRangeDr);
        out.set("reportJson", m_result.toJson());

        val channels = val::array();
        for (std::size_t i = 0; i < m_result.channels.size(); ++i) {
            channels.set(i, channelToVal(m_result.channels[i]));
        }
        out.set("channels", channels);

        if (m_result.stereo.has_value()) {
            const auto& s      = *m_result.stereo;
            val         stereo = val::object();
            stereo.set("correlation", s.correlation);
            stereo.set("balanceDb", s.balanceDb);
            stereo.set("monoCompatibilityDb", s.monoCompatibilityDb);
            stereo.set("correlationSeriesPtr", ptrOf(s.correlationSeries.data()));
            stereo.set("correlationSeriesCount", static_cast<std::uint32_t>(s.correlationSeries.size()));
            out.set("stereo", stereo);
        } else {
            out.set("stereo", val::null());
        }

        out.set("rmsSeriesPtr", ptrOf(m_result.rmsSeries.data()));
        out.set("rmsSeriesCount", static_cast<std::uint32_t>(m_result.rmsSeries.size()));
        out.set("rmsSeriesChannelCount", m_result.rmsSeriesChannelCount);

        // Same interleaving/grid as rmsSeries — feeds M10's digital-silence mode.
        out.set("allZeroSeriesPtr", ptrOf(m_result.allZeroSeries.data()));
        out.set("allZeroSeriesCount", static_cast<std::uint32_t>(m_result.allZeroSeries.size()));

        return out;
    }

    static val channelToVal(const aud::statistics::ChannelStatistics& c) {
        val out = val::object();
        out.set("peak", c.peak);
        out.set("peakDbfs", c.peakDbfs);
        out.set("peakFrame", static_cast<double>(c.peakFrame));
        out.set("minValue", c.minValue);
        out.set("maxValue", c.maxValue);
        out.set("dcOffset", c.dcOffset);
        out.set("rms", c.rms);
        out.set("rmsDbfs", c.rmsDbfs);
        out.set("variance", c.variance);
        out.set("stdDev", c.stdDev);
        out.set("crestFactorDb", c.crestFactorDb);
        out.set("zeroCrossingRate", c.zeroCrossingRate);

        if (c.bitDepth.effectiveBitDepth.has_value()) {
            out.set("effectiveBitDepth", *c.bitDepth.effectiveBitDepth);
        } else {
            out.set("effectiveBitDepth", val::null());
        }
        out.set("containerBitDepth", c.bitDepth.containerBitDepth);
        out.set("ditherLikely", c.bitDepth.ditherLikely);
        out.set("ditherConfidence", c.bitDepth.ditherConfidence);
        out.set("bitDepthDescription", c.bitDepth.describe());

        out.set("histogramPtr", ptrOf(c.histogram.data()));
        out.set("histogramCount", static_cast<std::uint32_t>(c.histogram.size()));
        return out;
    }

    const aud::AudioBuffer*             m_buffer;
    aud::statistics::StatisticsResult   m_result;
    std::unique_ptr<aud::Analyzer>      m_analyzer;
    std::size_t                         m_nextChunk    = 0;
    aud::FrameIndex                     m_frameCursor  = 0;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_statistics) {
    emscripten::class_<bindings::StatisticsHandle>("Statistics")
        .class_function("create", &bindings::StatisticsHandle::create)
        .function("processAvailableChunks", &bindings::StatisticsHandle::processAvailableChunks)
        .function("finish", &bindings::StatisticsHandle::finish);
}

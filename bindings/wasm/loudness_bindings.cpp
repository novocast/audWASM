// Embind surface for M08's loudness engine. Driven directly against an existing aud::AudioBuffer
// (the same non-owning audioBufferHandle handoff WaveformHandle uses in waveform_bindings.cpp),
// polling processAvailableChunks() as decode progresses and calling finish() once at the end —
// same shape as Waveform's binding.
//
// Interacts with LoudnessAnalyzer only through the base aud::Analyzer* interface, and owns its
// own LoudnessResult directly (a plain struct, no vtable) rather than ever naming the concrete
// LoudnessAnalyzer type — see loudness_analyzer.hpp's makeLoudnessAnalyzer() comment for why
// directly constructing/destroying it from this (RTTI-enabled) Embind TU would fail to link
// against aud_core's -fno-rtti compile of it.
//
// Time series (momentary/short-term LUFS) and per-channel peaks are handed back as {ptr, length}
// heap views per M01's binding convention, never per-sample calls.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../../engine/analysis/loudness/loudness_analyzer.hpp"
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

class LoudnessHandle {
public:
    // `oversampling` must be 4, 8 or 16 (0 defaults to 4, the spec-compliant minimum).
    static std::unique_ptr<LoudnessHandle> create(std::uintptr_t audioBufferHandle, std::uint32_t oversampling) {
        const auto* buffer = reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle);
        if (buffer == nullptr) {
            return nullptr;
        }

        aud::loudness::LoudnessConfig config;
        config.truePeakOversampling = oversampling == 0 ? 4 : oversampling;

        auto handle = std::unique_ptr<LoudnessHandle>(new LoudnessHandle(buffer, config));
        const aud::AudioSpec spec{buffer->sampleRate(), buffer->channelCount(), buffer->frameCount()};
        if (!handle->m_analyzer->begin(spec).has_value()) {
            return nullptr;
        }
        return handle;
    }

    // Feeds every AudioBuffer chunk appended since the last call — safe to call repeatedly
    // alongside progressive decode, same polling contract as WaveformHandle::processAvailableChunks.
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

    // Runs the two-stage gate and LRA percentile method; call once after decode is complete.
    val finish() {
        auto result = m_analyzer->finish();
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        return resultToVal();
    }

    double gainToTargetDb(double targetLufs) const { return m_result.gainToTargetDb(targetLufs); }

private:
    LoudnessHandle(const aud::AudioBuffer* buffer, const aud::loudness::LoudnessConfig& config)
        : m_buffer(buffer), m_analyzer(aud::loudness::makeLoudnessAnalyzer(m_result, config)) {}

    val resultToVal() const {
        val out = val::object();
        out.set("ok", true);
        out.set("integratedLufs", m_result.integratedLufs);
        out.set("loudnessRangeLu", m_result.loudnessRangeLu);
        out.set("truePeakDbtp", m_result.truePeakDbtp);
        out.set("samplePeakDbfs", m_result.samplePeakDbfs);
        out.set("truePeakFrame", static_cast<double>(m_result.truePeakFrame));
        out.set("truePeakOversampling", m_result.truePeakOversampling);
        out.set("usedFallbackChannelLayout", m_result.usedFallbackChannelLayout);

        out.set("truePeakPerChannelPtr", ptrOf(m_result.truePeakPerChannelDbtp.data()));
        out.set("truePeakPerChannelCount", static_cast<std::uint32_t>(m_result.truePeakPerChannelDbtp.size()));
        out.set("samplePeakPerChannelPtr", ptrOf(m_result.samplePeakPerChannelDbfs.data()));
        out.set("samplePeakPerChannelCount", static_cast<std::uint32_t>(m_result.samplePeakPerChannelDbfs.size()));

        out.set("momentaryPtr", ptrOf(m_result.momentaryLufs.data()));
        out.set("momentaryCount", static_cast<std::uint32_t>(m_result.momentaryLufs.size()));
        out.set("shortTermPtr", ptrOf(m_result.shortTermLufs.data()));
        out.set("shortTermCount", static_cast<std::uint32_t>(m_result.shortTermLufs.size()));
        return out;
    }

    const aud::AudioBuffer*        m_buffer;
    aud::loudness::LoudnessResult  m_result;
    std::unique_ptr<aud::Analyzer> m_analyzer;
    std::size_t                    m_nextChunk = 0;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_loudness) {
    emscripten::class_<bindings::LoudnessHandle>("Loudness")
        .class_function("create", &bindings::LoudnessHandle::create)
        .function("processAvailableChunks", &bindings::LoudnessHandle::processAvailableChunks)
        .function("finish", &bindings::LoudnessHandle::finish)
        .function("gainToTargetDb", &bindings::LoudnessHandle::gainToTargetDb);
}

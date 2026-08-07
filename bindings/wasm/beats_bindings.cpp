// Embind surface for M13's beat detector. Same non-owning audioBufferHandle handoff and
// processAvailableChunks()/finish() polling contract as Waveform/Loudness/Statistics/Clipping/Dc —
// driven directly against an existing aud::AudioBuffer, bypassing the Analyzer registry (M20 isn't
// built yet).
//
// The retained ODF (doc: "Retaining the ODF is deliberate") is handed back as a {ptr,count} heap
// view, same convention as Dc's windowSeries/Statistics's rmsSeries; onsets/beats/alternatives are
// small per-event arrays, so they go back as val::arrays of val::objects like Silence's regions.
//
// applyEdits() is the manual-edit merge (doc's "Editability"): it never mutates the detected
// result, only returns a new merged view — matching aud::beats::applyManualEdits().

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../../engine/analysis/beats/beat_analyzer.hpp"
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

val onsetToVal(const aud::beats::Onset& o) {
    val out = val::object();
    out.set("timeSeconds", o.timeSeconds);
    out.set("frame", static_cast<double>(o.frame));
    out.set("strength", o.strength);
    out.set("bandMask", static_cast<std::uint32_t>(o.bandMask));
    return out;
}

val beatToVal(const aud::beats::Beat& b) {
    val out = val::object();
    out.set("timeSeconds", b.timeSeconds);
    out.set("frame", static_cast<double>(b.frame));
    out.set("confidence", b.confidence);
    out.set("beatIndexInBar", b.beatIndexInBar);
    return out;
}

val resultToVal(const aud::beats::BeatResult& result) {
    val out = val::object();
    out.set("ok", true);
    out.set("primaryBpm", result.primaryBpm);
    out.set("tempoConfidence", result.tempoConfidence);
    out.set("phaseConfidence", result.phaseConfidence);
    out.set("tempoIsStable", result.tempoIsStable);
    out.set("odfHopSeconds", result.odfHopSeconds);
    out.set("reportJson", result.toJson());

    val onsets = val::array();
    for (std::size_t i = 0; i < result.onsets.size(); ++i) onsets.set(i, onsetToVal(result.onsets[i]));
    out.set("onsets", onsets);

    val beats = val::array();
    for (std::size_t i = 0; i < result.beats.size(); ++i) beats.set(i, beatToVal(result.beats[i]));
    out.set("beats", beats);

    val alternatives = val::array();
    for (std::size_t i = 0; i < result.alternatives.size(); ++i) {
        val alt = val::object();
        alt.set("bpm", result.alternatives[i].bpm);
        alt.set("score", result.alternatives[i].score);
        alternatives.set(i, alt);
    }
    out.set("alternatives", alternatives);

    // odf/tempoSeries are potentially long-ish (one value per STFT frame / per 10s window) — heap
    // views, not copied through val, same convention as Dc's windowSeries.
    out.set("odfPtr", ptrOf(result.odf.data()));
    out.set("odfCount", static_cast<std::uint32_t>(result.odf.size()));
    out.set("tempoSeriesPtr", ptrOf(result.tempoSeries.data()));
    out.set("tempoSeriesCount", static_cast<std::uint32_t>(result.tempoSeries.size()));

    return out;
}

}  // namespace

class BeatsHandle {
public:
    static std::unique_ptr<BeatsHandle> create(std::uintptr_t audioBufferHandle) {
        const auto* buffer = reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle);
        if (buffer == nullptr) return nullptr;

        auto handle = std::unique_ptr<BeatsHandle>(new BeatsHandle(buffer));
        const aud::AudioSpec spec{buffer->sampleRate(), buffer->channelCount(), buffer->frameCount()};
        if (!handle->m_analyzer->begin(spec).has_value()) return nullptr;
        return handle;
    }

    // Re-begins with new parameters — must be called (if at all) before the first
    // processAvailableChunks(), same convention as Dc::configure()/Clipping::configure().
    void configure(std::uint32_t fftSize, std::uint32_t hopSize, std::int32_t timeSignatureBeatsPerBar) {
        if (fftSize > 0) m_config.fftSize = fftSize;
        if (hopSize > 0) m_config.hopSize = hopSize;
        if (timeSignatureBeatsPerBar > 0) m_config.timeSignatureBeatsPerBar = timeSignatureBeatsPerBar;

        m_analyzer = aud::beats::makeBeatAnalyzer(m_result, m_config);
        const aud::AudioSpec spec{m_buffer->sampleRate(), m_buffer->channelCount(), m_buffer->frameCount()};
        m_analyzer->begin(spec);
        m_nextChunk = 0;
    }

    // Feeds every AudioBuffer chunk appended since the last call — safe to call repeatedly
    // alongside progressive decode, same polling contract as Waveform/Loudness/Statistics/Dc.
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
            if (!result.has_value()) return errorToVal(result.error());
            ++m_nextChunk;
        }
        return okVal();
    }

    val finish() {
        auto result = m_analyzer->finish();
        if (!result.has_value()) return errorToVal(result.error());
        m_detected = m_result;
        return resultToVal(m_result);
    }

    // Manual-edit merge (doc's "Editability") — never mutates the detected result. `addedPtr`/
    // `removedPtr` point at HEAPF64 arrays of timeSeconds; pass count 0 / ptr 0 for "none".
    val applyEdits(double tempoOverrideBpm, double phaseNudgeSeconds, double downbeatTimeSeconds,
                   bool hasDownbeat, std::uintptr_t addedPtr, std::uint32_t addedCount, std::uintptr_t removedPtr,
                   std::uint32_t removedCount, std::int32_t timeSignatureBeatsPerBar) {
        aud::beats::BeatEdits edits;
        if (tempoOverrideBpm > 0.0) edits.tempoOverrideBpm = tempoOverrideBpm;
        edits.phaseNudgeSeconds = phaseNudgeSeconds;
        if (hasDownbeat) edits.downbeatTimeSeconds = downbeatTimeSeconds;
        edits.timeSignatureBeatsPerBar = timeSignatureBeatsPerBar;

        if (addedPtr != 0 && addedCount > 0) {
            const auto* p = reinterpret_cast<const double*>(addedPtr);
            edits.addedBeatSeconds.assign(p, p + addedCount);
        }
        if (removedPtr != 0 && removedCount > 0) {
            const auto* p = reinterpret_cast<const double*>(removedPtr);
            edits.removedBeatSeconds.assign(p, p + removedCount);
        }

        const auto merged = aud::beats::applyManualEdits(m_detected, edits);
        return resultToVal(merged);
    }

private:
    explicit BeatsHandle(const aud::AudioBuffer* buffer)
        : m_buffer(buffer), m_analyzer(aud::beats::makeBeatAnalyzer(m_result, m_config)) {}

    const aud::AudioBuffer*        m_buffer;
    aud::beats::BeatConfig          m_config;
    aud::beats::BeatResult          m_result;
    aud::beats::BeatResult          m_detected;  // snapshot taken at finish(), edits merge on top of this
    std::unique_ptr<aud::Analyzer> m_analyzer;
    std::size_t                    m_nextChunk = 0;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_beats) {
    emscripten::class_<bindings::BeatsHandle>("Beats")
        .class_function("create", &bindings::BeatsHandle::create)
        .function("configure", &bindings::BeatsHandle::configure)
        .function("processAvailableChunks", &bindings::BeatsHandle::processAvailableChunks)
        .function("finish", &bindings::BeatsHandle::finish)
        .function("applyEdits", &bindings::BeatsHandle::applyEdits);
}

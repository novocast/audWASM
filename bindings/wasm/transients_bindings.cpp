// Embind surface for M14's transient detector. Same non-owning audioBufferHandle handoff and
// processAvailableChunks()/finish() polling contract as Waveform/Loudness/Statistics/Clipping/Dc/
// Beats, driven directly against an existing aud::AudioBuffer, bypassing the Analyzer registry
// (M20 isn't built yet).
//
// M14 consumes M13's onset list as candidates rather than running a second onset detector (see
// transient_analyzer.hpp's header comment) — `create()` takes the candidate onset times/strengths
// as bulk {ptr,count} arrays (HEAPF64 for times, HEAPF32 for strengths), the same convention as
// Beats::applyEdits()'s addedPtr/removedPtr, so the JS side just runs Beats first and hands its
// onsets straight through.
//
// Musical transients and defects (clicks/dropouts) go back as separate val::arrays, matching the
// doc's "surfaced separately in both the data and the UI" decision.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../../engine/analysis/transients/transient_analyzer.hpp"
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

const char* classToString(aud::transients::TransientClass klass) {
    using aud::transients::TransientClass;
    switch (klass) {
        case TransientClass::Kick: return "kick";
        case TransientClass::Snare: return "snare";
        case TransientClass::HiHat: return "hiHat";
        case TransientClass::Percussion: return "percussion";
        case TransientClass::TonalOnset: return "tonalOnset";
        case TransientClass::Click: return "click";
        case TransientClass::Dropout: return "dropout";
        case TransientClass::Unclassified: return "unclassified";
    }
    return "unclassified";
}

val transientToVal(const aud::transients::Transient& t) {
    val out = val::object();
    out.set("startFrame", static_cast<double>(t.startFrame));
    out.set("attackFrame", static_cast<double>(t.attackFrame));
    out.set("startSeconds", t.startSeconds);
    out.set("attackSeconds", t.attackSeconds);
    out.set("classification", std::string(classToString(t.classification)));
    out.set("classConfidence", t.classConfidence);
    out.set("strength", t.strength);
    out.set("peakDbfs", t.peakDbfs);
    out.set("attackTimeMs", t.attackTimeMs);
    out.set("decayTimeMs", t.decayTimeMs);
    out.set("spectralCentroidHz", t.spectralCentroidHz);
    out.set("spectralFlatness", t.spectralFlatness);

    val bands = val::array();
    for (std::size_t i = 0; i < t.bandEnergyRatio.size(); ++i) bands.set(i, t.bandEnergyRatio[i]);
    out.set("bandEnergyRatio", bands);

    return out;
}

val resultToVal(const aud::transients::TransientResult& result) {
    val out = val::object();
    out.set("ok", true);
    out.set("reportJson", result.toJson());

    val transients = val::array();
    for (std::size_t i = 0; i < result.transients.size(); ++i) transients.set(i, transientToVal(result.transients[i]));
    out.set("transients", transients);

    val defects = val::array();
    for (std::size_t i = 0; i < result.defects.size(); ++i) defects.set(i, transientToVal(result.defects[i]));
    out.set("defects", defects);

    val countByClass = val::array();
    for (std::size_t i = 0; i < result.countByClass.size(); ++i) countByClass.set(i, result.countByClass[i]);
    out.set("countByClass", countByClass);

    return out;
}

}  // namespace

class TransientsHandle {
public:
    // `onsetTimesPtr`/`onsetStrengthsPtr` point at HEAPF64/HEAPF32 arrays of length `onsetCount`
    // (M13's Beats::onsets, already computed by the caller) — pass count 0 / ptr 0 for none.
    static std::unique_ptr<TransientsHandle> create(std::uintptr_t audioBufferHandle, std::uintptr_t onsetTimesPtr,
                                                       std::uintptr_t onsetStrengthsPtr, std::uint32_t onsetCount) {
        const auto* buffer = reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle);
        if (buffer == nullptr) return nullptr;

        std::vector<aud::transients::TransientCandidate> candidates;
        candidates.reserve(onsetCount);
        const auto* times      = reinterpret_cast<const double*>(onsetTimesPtr);
        const auto* strengths  = reinterpret_cast<const float*>(onsetStrengthsPtr);
        for (std::uint32_t i = 0; i < onsetCount; ++i) {
            aud::transients::TransientCandidate candidate;
            candidate.timeSeconds = times != nullptr ? times[i] : 0.0;
            candidate.strength    = strengths != nullptr ? strengths[i] : 0.0f;
            candidates.push_back(candidate);
        }

        auto handle = std::unique_ptr<TransientsHandle>(new TransientsHandle(buffer, std::move(candidates)));
        const aud::AudioSpec spec{buffer->sampleRate(), buffer->channelCount(), buffer->frameCount()};
        if (!handle->m_analyzer->begin(spec).has_value()) return nullptr;
        return handle;
    }

    // Feeds every AudioBuffer chunk appended since the last call — safe to call repeatedly
    // alongside progressive decode, same polling contract as Waveform/Loudness/Statistics/Dc/Beats.
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
        return resultToVal(m_result);
    }

private:
    TransientsHandle(const aud::AudioBuffer* buffer, std::vector<aud::transients::TransientCandidate> candidates)
        : m_buffer(buffer),
          m_analyzer(aud::transients::makeTransientAnalyzer(m_result, std::move(candidates))) {}

    const aud::AudioBuffer*             m_buffer;
    aud::transients::TransientResult    m_result;
    std::unique_ptr<aud::Analyzer>      m_analyzer;
    std::size_t                         m_nextChunk = 0;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_transients) {
    emscripten::class_<bindings::TransientsHandle>("Transients")
        .class_function("create", &bindings::TransientsHandle::create)
        .function("processAvailableChunks", &bindings::TransientsHandle::processAvailableChunks)
        .function("finish", &bindings::TransientsHandle::finish);
}

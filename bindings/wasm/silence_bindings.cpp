// Embind surface for M10's silence detector. Unlike Waveform/Loudness/Statistics, SilenceHandle
// does not itself stream PCM through an Analyzer: per M10's "re-running on parameter change"
// design, detect() is a pure, instant function over series already computed by M09/M08 (copied in
// once at create() time), so a threshold slider can call it on every tick. refineThreshold()/
// refineDigital() are the separate, deliberately PCM-touching step — call those once, debounced,
// after the user stops dragging.
//
// Regions are handed back as a val::array of small val::objects (like Statistics's per-channel
// stats), not {ptr,count} — region counts are small, unlike the RMS/histogram series.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cmath>
#include <cstdint>
#include <memory>

#include "../../engine/analysis/silence/boundary_refine.hpp"
#include "../../engine/analysis/silence/silence_detector.hpp"
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

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

val regionToVal(const aud::silence::SilenceRegion& r) {
    val out = val::object();
    out.set("beginFrame", static_cast<double>(r.range.begin));
    out.set("endFrame", static_cast<double>(r.range.end));
    out.set("startSeconds", r.startSeconds);
    out.set("endSeconds", r.endSeconds);
    out.set("kind", static_cast<std::uint32_t>(r.kind));
    out.set("position", static_cast<std::uint32_t>(r.position));
    out.set("peakDbfsWithin", r.peakDbfsWithin);
    out.set("rmsDbfsWithin", r.rmsDbfsWithin);
    out.set("channelMask", r.channelMask);
    return out;
}

val resultToVal(const aud::silence::SilenceResult& result) {
    val out = val::object();
    out.set("ok", true);

    val regions = val::array();
    for (std::size_t i = 0; i < result.regions.size(); ++i) {
        regions.set(i, regionToVal(result.regions[i]));
    }
    out.set("regions", regions);
    out.set("leadingSilenceSeconds", result.leadingSilenceSeconds);
    out.set("trailingSilenceSeconds", result.trailingSilenceSeconds);
    out.set("totalSilenceSeconds", result.totalSilenceSeconds);
    out.set("silenceFraction", result.silenceFraction);

    val params = val::object();
    params.set("thresholdDb", result.parametersUsed.thresholdDb);
    params.set("minDurationMs", result.parametersUsed.minDurationMs);
    params.set("mergeGapMs", result.parametersUsed.mergeGapMs);
    params.set("channelModeAny", result.parametersUsed.channelMode == aud::silence::ChannelMode::Any);
    params.set("useHysteresis", result.parametersUsed.useHysteresis);
    params.set("hysteresisDb", result.parametersUsed.hysteresisDb);
    out.set("parametersUsed", params);

    return out;
}

}  // namespace

class SilenceHandle {
public:
    // Copies the already-computed M09 RMS/digital-silence series and M08 momentary loudness
    // series in once — cheap relative to computing them, and lets detect() run with no further
    // pointer chasing on every slider tick. Pointers/counts of 0 are treated as "not available"
    // (e.g. no loudness pass run yet), and the corresponding detect mode then reports nothing.
    static std::unique_ptr<SilenceHandle> create(std::uintptr_t audioBufferHandle, std::uintptr_t rmsSeriesPtr,
                                                    std::uint32_t rmsSeriesCount, std::uint32_t rmsChannelCount,
                                                    std::uintptr_t allZeroSeriesPtr, std::uint32_t allZeroSeriesCount,
                                                    std::uintptr_t momentaryLufsPtr, std::uint32_t momentaryLufsCount) {
        const auto* buffer = reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle);
        if (buffer == nullptr) return nullptr;

        auto handle = std::unique_ptr<SilenceHandle>(new SilenceHandle(buffer));
        auto& input = handle->m_input;

        input.sampleRate        = buffer->sampleRate();
        input.frameCount        = buffer->frameCount();
        input.channelCount      = rmsChannelCount;
        input.rmsWindowSeconds  = 0.05;  // fixed by M09 (sampleRate/20)

        if (rmsSeriesPtr != 0 && rmsSeriesCount > 0) {
            const auto* p = reinterpret_cast<const float*>(rmsSeriesPtr);
            input.rmsSeries.assign(p, p + rmsSeriesCount);
        }
        if (allZeroSeriesPtr != 0 && allZeroSeriesCount > 0) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(allZeroSeriesPtr);
            input.digitalSilenceSeries.assign(p, p + allZeroSeriesCount);
        }
        if (momentaryLufsPtr != 0 && momentaryLufsCount > 0) {
            const auto* p = reinterpret_cast<const float*>(momentaryLufsPtr);
            input.momentaryLufs.assign(p, p + momentaryLufsCount);
        }
        input.momentaryLufsWindowSeconds = 0.1;  // fixed by M08

        return handle;
    }

    // O(windows), instant — safe to call on every slider tick (M10 "reparameterisation recomputes
    // from the retained RMS series, instantly, on the main thread"). Runs all three modes; the UI
    // picks which to show.
    val detect(double thresholdDb, double minDurationMs, double mergeGapMs, bool channelModeAny,
               bool useHysteresis, double hysteresisDb, double perceptualGateLufs) {
        m_lastParams.thresholdDb   = thresholdDb;
        m_lastParams.minDurationMs = minDurationMs;
        m_lastParams.mergeGapMs    = mergeGapMs;
        m_lastParams.channelMode   = channelModeAny ? aud::silence::ChannelMode::Any : aud::silence::ChannelMode::All;
        m_lastParams.useHysteresis = useHysteresis;
        m_lastParams.hysteresisDb  = hysteresisDb;
        m_lastGateLufs             = perceptualGateLufs;

        m_lastThreshold  = aud::silence::SilenceDetector::detectThreshold(m_input, m_lastParams);
        m_lastDigital    = aud::silence::SilenceDetector::detectDigital(m_input, m_lastParams);
        m_lastPerceptual = aud::silence::SilenceDetector::detectPerceptual(m_input, m_lastParams, m_lastGateLufs);

        val out = val::object();
        out.set("ok", true);
        out.set("threshold", resultToVal(m_lastThreshold));
        out.set("digital", resultToVal(m_lastDigital));
        out.set("perceptual", resultToVal(m_lastPerceptual));
        return out;
    }

    // Sample-precise boundaries + exact level stats for the most recent detect()'s threshold
    // regions. PCM-touching (M10: defer until dragging stops) — must be called after detect().
    val refineThreshold() {
        return refine(m_lastThreshold, dbToLinear(m_lastParams.thresholdDb));
    }

    // Same, for the digital-silence regions from the most recent detect() (exact-zero test).
    val refineDigital() { return refine(m_lastDigital, 0.0); }

private:
    explicit SilenceHandle(const aud::AudioBuffer* buffer) : m_buffer(buffer) {}

    val refine(aud::silence::SilenceResult& result, double thresholdLinear) {
        const std::size_t windowFrames = m_buffer->sampleRate() == 0 ? 0 : m_buffer->sampleRate() / 20;
        auto refineResult = aud::silence::refineRegionBoundaries(*m_buffer, result.regions, windowFrames,
                                                                    thresholdLinear, m_lastParams.channelMode);
        if (!refineResult.has_value()) {
            return errorToVal(refineResult.error());
        }
        return resultToVal(result);
    }

    const aud::AudioBuffer*          m_buffer;
    aud::silence::SilenceInput       m_input;
    aud::silence::SilenceParameters  m_lastParams;
    double                            m_lastGateLufs = -70.0;
    aud::silence::SilenceResult       m_lastThreshold;
    aud::silence::SilenceResult       m_lastDigital;
    aud::silence::SilenceResult       m_lastPerceptual;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_silence) {
    emscripten::class_<bindings::SilenceHandle>("Silence")
        .class_function("create", &bindings::SilenceHandle::create)
        .function("detect", &bindings::SilenceHandle::detect)
        .function("refineThreshold", &bindings::SilenceHandle::refineThreshold)
        .function("refineDigital", &bindings::SilenceHandle::refineDigital);
}

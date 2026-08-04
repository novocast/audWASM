// Embind surface for M03's playback engine. Architecture note (deviation from the M03 design doc's
// sketch, recorded here since it's a real decision, not an oversight): the doc's diagram shows the
// Resampler living inside the AudioWorklet's own WASM instance, with the ring buffer carrying raw
// source-rate PCM. We instead run the Resampler and Transport together in the *producer* context
// (the same worker/module instance that already owns decode, per M02), producing already
// device-rate, gain-applied audio into TransportHandle's internal ring; the AudioWorkletProcessor
// is then a plain-JS/TS consumer with no WASM instance of its own. This sidesteps instantiating a
// second Emscripten module inside AudioWorkletGlobalScope (synchronous-module-loading and
// threading caveats there are real, see M03 "Risks" re: Safari worklet quirks) while still meeting
// every acceptance criterion in the doc (underrun-free playback, <10ms seek, ±5ms position, -80dB
// THD+N, click-free transitions, works with and without cross-origin isolation) — the M03 doc's own
// open-question section explicitly flags "resample the whole buffer once" vs. "resample in the
// worklet" as an undecided tradeoff, so this resolves it rather than contradicting it. Revisit if
// real-world profiling shows the worker/main-thread resample step can't keep up.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../../engine/playback/transport.hpp"
#include "../../engine/util/audio_buffer.hpp"

using emscripten::val;

namespace bindings {

namespace {

const char* statusToString(aud::playback::TransportStatus status) noexcept {
    using S = aud::playback::TransportStatus;
    switch (status) {
        case S::Idle: return "idle";
        case S::Loading: return "loading";
        case S::Ready: return "ready";
        case S::Playing: return "playing";
        case S::Paused: return "paused";
        case S::Seeking: return "seeking";
        case S::Ended: return "ended";
    }
    return "unknown";
}

val dispatchResultToVal(const aud::Result<void>& result) {
    val out = val::object();
    out.set("ok", result.has_value());
    if (!result.has_value()) {
        out.set("code", std::string(aud::toString(result.error().code)));
        out.set("detail", result.error().detail);
    }
    return out;
}

}  // namespace

// Thin JS-facing wrapper around aud::playback::Transport + its production ring buffer. See the
// file header for why resampling happens here rather than in a worklet-side WASM instance.
class TransportHandle {
public:
    static std::unique_ptr<TransportHandle> create(std::uint32_t sourceRate, std::uint32_t outputSampleRate,
                                                     std::uint32_t channels, std::uint32_t ringCapacityFrames) {
        if (sourceRate == 0 || outputSampleRate == 0 || channels == 0 || ringCapacityFrames == 0) {
            return nullptr;
        }
        return std::unique_ptr<TransportHandle>(
            new TransportHandle(sourceRate, outputSampleRate, channels, ringCapacityFrames));
    }

    // `audioBufferHandle` is DecodeSessionHandle::audioBufferHandle()'s return value — an opaque
    // pointer to the aud::AudioBuffer the decode session owns. Non-owning: the caller (JS) must
    // keep the DecodeSession alive at least as long as this Transport uses it.
    void attachSource(std::uintptr_t audioBufferHandle) {
        m_transport.setSource(reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle));
    }

    void setSourceComplete(bool complete) { m_transport.setSourceComplete(complete); }

    val dispatchLoad() { return dispatch(aud::playback::TransportEvent::load()); }
    val dispatchReady(double durationFrames) {
        return dispatch(aud::playback::TransportEvent::readySignal(static_cast<aud::FrameIndex>(durationFrames)));
    }
    val dispatchPlay() { return dispatch(aud::playback::TransportEvent::play()); }
    val dispatchPause() { return dispatch(aud::playback::TransportEvent::pause()); }
    val dispatchSeekTo(double targetFrame) {
        return dispatch(aud::playback::TransportEvent::seekTo(static_cast<aud::FrameIndex>(targetFrame)));
    }
    val dispatchSetLoopRange(double beginFrame, double endFrame) {
        return dispatch(aud::playback::TransportEvent::setLoopRange(
            aud::FrameRange{static_cast<aud::FrameIndex>(beginFrame), static_cast<aud::FrameIndex>(endFrame)}));
    }
    val dispatchSetLoopEnabled(bool enabled) {
        return dispatch(aud::playback::TransportEvent::setLoopEnabled(enabled));
    }
    val dispatchSetLoopCrossfadeFrames(double frames) {
        return dispatch(
            aud::playback::TransportEvent::setLoopCrossfadeFrames(static_cast<aud::FrameIndex>(frames)));
    }
    val dispatchSetGain(float gain) { return dispatch(aud::playback::TransportEvent::setGain(gain)); }
    val dispatchReset() { return dispatch(aud::playback::TransportEvent::reset()); }

    // Pulls up to `maxFrames` of resampled audio from the source into the internal ring. The
    // producer context (worker or main thread) calls this whenever it has idle time — analogous to
    // decodeWorker.ts's feed loop — to keep the ring topped up ahead of the audio thread's demand.
    std::uint32_t pump(std::uint32_t maxFrames) {
        return static_cast<std::uint32_t>(m_transport.produceInto(m_ring, maxFrames));
    }

    // Copies up to `framesRequested` frames out of the ring, with the current monitoring gain
    // applied, into a caller-allocated WASM-heap region at `outPtr` — planar-contiguous per
    // channel (`out[ch * framesRequested + i]`), matching AudioWorkletProcessor's per-channel
    // Float32Array output layout. Returns frames actually available (a return value below
    // `framesRequested` is an underrun; the caller must silence-fill the remainder and count a
    // dropout, per M03's risk table).
    std::uint32_t renderInto(std::uintptr_t outPtr, std::uint32_t framesRequested) {
        auto* out = reinterpret_cast<aud::Sample*>(outPtr);

        m_scratch.assign(m_channels, std::vector<aud::Sample>(framesRequested));
        std::vector<std::span<aud::Sample>> planarOut(m_channels);
        for (std::uint32_t ch = 0; ch < m_channels; ++ch) {
            planarOut[ch] = std::span<aud::Sample>(m_scratch[ch]);
        }

        const std::size_t got = m_ring.read(std::span<std::span<aud::Sample>>(planarOut), framesRequested);
        const float       gain = m_transport.state().gain;

        for (std::uint32_t ch = 0; ch < m_channels; ++ch) {
            aud::Sample* dst = out + static_cast<std::size_t>(ch) * framesRequested;
            for (std::size_t i = 0; i < got; ++i) {
                dst[i] = m_scratch[ch][i] * gain;
            }
            for (std::size_t i = got; i < framesRequested; ++i) {
                dst[i] = 0.0f;
            }
        }
        return static_cast<std::uint32_t>(got);
    }

    val getState() const {
        const auto& s   = m_transport.state();
        val         out = val::object();
        out.set("status", std::string(statusToString(s.status)));
        out.set("positionFrames", static_cast<double>(m_transport.producedSourceFrame()));
        out.set("durationFrames", static_cast<double>(s.durationFrames));
        out.set("loopEnabled", s.loop.enabled);
        out.set("loopBegin", static_cast<double>(s.loop.range.begin));
        out.set("loopEnd", static_cast<double>(s.loop.range.end));
        out.set("gain", s.gain);
        out.set("outputSampleRate", s.outputSampleRate);
        return out;
    }

    std::uint32_t ringFramesAvailable() const {
        return static_cast<std::uint32_t>(m_ring.framesAvailableToRead());
    }

private:
    TransportHandle(std::uint32_t sourceRate, std::uint32_t outputSampleRate, std::uint32_t channels,
                     std::uint32_t ringCapacityFrames)
        : m_channels(channels), m_transport(channels, sourceRate, outputSampleRate), m_ring(channels, ringCapacityFrames) {}

    val dispatch(const aud::playback::TransportEvent& event) {
        return dispatchResultToVal(m_transport.dispatch(event));
    }

    std::uint32_t                          m_channels;
    aud::playback::Transport               m_transport;
    aud::playback::RingBuffer              m_ring;
    std::vector<std::vector<aud::Sample>>  m_scratch;  // renderInto() scratch, reused across calls
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_playback) {
    emscripten::class_<bindings::TransportHandle>("Transport")
        .class_function("create", &bindings::TransportHandle::create)
        .function("attachSource", &bindings::TransportHandle::attachSource)
        .function("setSourceComplete", &bindings::TransportHandle::setSourceComplete)
        .function("dispatchLoad", &bindings::TransportHandle::dispatchLoad)
        .function("dispatchReady", &bindings::TransportHandle::dispatchReady)
        .function("dispatchPlay", &bindings::TransportHandle::dispatchPlay)
        .function("dispatchPause", &bindings::TransportHandle::dispatchPause)
        .function("dispatchSeekTo", &bindings::TransportHandle::dispatchSeekTo)
        .function("dispatchSetLoopRange", &bindings::TransportHandle::dispatchSetLoopRange)
        .function("dispatchSetLoopEnabled", &bindings::TransportHandle::dispatchSetLoopEnabled)
        .function("dispatchSetLoopCrossfadeFrames", &bindings::TransportHandle::dispatchSetLoopCrossfadeFrames)
        .function("dispatchSetGain", &bindings::TransportHandle::dispatchSetGain)
        .function("dispatchReset", &bindings::TransportHandle::dispatchReset)
        .function("pump", &bindings::TransportHandle::pump)
        .function("renderInto", &bindings::TransportHandle::renderInto)
        .function("getState", &bindings::TransportHandle::getState)
        .function("ringFramesAvailable", &bindings::TransportHandle::ringFramesAvailable);
}

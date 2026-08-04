#pragma once

// Transport state machine + source-frame accounting + production driver. See M03 "Transport state
// machine": every transition (UI, the M19 debugger, and tests) goes through the single dispatch()
// reducer so all three observe the same sequence, and illegal transitions are logged rather than
// silently ignored.
//
// Transport is the producer-side owner: it knows how to pull frames from the decoded AudioBuffer
// (which may still be growing — progressive decode, M02), resample them to the output device rate,
// and push them into a RingBuffer for the audio thread to consume. It does not itself touch the
// audio thread's render loop (that side only ever reads the RingBuffer + applies gain/fades — see
// M03 "Seeking"/"Click-free everything" for why muting/fading is a consumer-side concern keyed off
// discontinuity events this class raises through state changes).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../util/audio_buffer.hpp"
#include "../util/audio_types.hpp"
#include "../util/result.hpp"
#include "resampler.hpp"
#include "ring_buffer.hpp"

namespace aud::playback {

enum class TransportStatus : std::uint8_t {
    Idle,
    Loading,
    Ready,
    Playing,
    Paused,
    Seeking,
    Ended,
};

struct LoopState {
    bool       enabled = false;
    FrameRange range{0, 0};
};

struct TransportState {
    TransportStatus status         = TransportStatus::Idle;
    FrameIndex      positionFrames = 0;
    FrameIndex      durationFrames = kNoFrame;
    LoopState       loop;
    double          rate             = 1.0;  // only 1.0 accepted in v1 (M03 "Playback rate")
    float           gain             = 1.0f;  // monitoring gain only, never affects analysis
    SampleRate      outputSampleRate = 0;
};

enum class TransportEventKind : std::uint8_t {
    Load,
    ReadySignal,
    Play,
    Pause,
    SeekTo,
    SetLoopRange,
    SetLoopEnabled,
    SetLoopCrossfadeFrames,
    SetRate,
    SetGain,
    Reset,
    EndReached,
};

struct TransportEvent {
    TransportEventKind kind;
    FrameIndex         frame = 0;  // ReadySignal's duration, SeekTo's target, SetLoopCrossfadeFrames
    FrameRange         range{0, 0};
    bool               flag = false;
    double             number = 0.0;
    float              gain   = 1.0f;

    static TransportEvent load() noexcept { return TransportEvent{TransportEventKind::Load}; }
    static TransportEvent readySignal(FrameIndex durationFrames) noexcept {
        TransportEvent e{TransportEventKind::ReadySignal};
        e.frame = durationFrames;
        return e;
    }
    static TransportEvent play() noexcept { return TransportEvent{TransportEventKind::Play}; }
    static TransportEvent pause() noexcept { return TransportEvent{TransportEventKind::Pause}; }
    static TransportEvent seekTo(FrameIndex targetFrame) noexcept {
        TransportEvent e{TransportEventKind::SeekTo};
        e.frame = targetFrame;
        return e;
    }
    static TransportEvent setLoopRange(FrameRange range) noexcept {
        TransportEvent e{TransportEventKind::SetLoopRange};
        e.range = range;
        return e;
    }
    static TransportEvent setLoopEnabled(bool enabled) noexcept {
        TransportEvent e{TransportEventKind::SetLoopEnabled};
        e.flag = enabled;
        return e;
    }
    static TransportEvent setLoopCrossfadeFrames(FrameIndex frames) noexcept {
        TransportEvent e{TransportEventKind::SetLoopCrossfadeFrames};
        e.frame = frames;
        return e;
    }
    static TransportEvent setRate(double rate) noexcept {
        TransportEvent e{TransportEventKind::SetRate};
        e.number = rate;
        return e;
    }
    static TransportEvent setGain(float gain) noexcept {
        TransportEvent e{TransportEventKind::SetGain};
        e.gain = gain;
        return e;
    }
    static TransportEvent reset() noexcept { return TransportEvent{TransportEventKind::Reset}; }
    static TransportEvent endReached() noexcept { return TransportEvent{TransportEventKind::EndReached}; }
};

class Transport {
public:
    Transport(ChannelIndex channels, SampleRate sourceRate, SampleRate outputSampleRate,
              Resampler::Quality quality = Resampler::Quality::Good);

    // Wires (or re-wires) the decoded source. Non-owning: per M02, PCM is worker-owned and outlives
    // the Transport's use of it for the duration of a playback session.
    void setSource(const AudioBuffer* buffer) noexcept { m_buffer = buffer; }

    // Signals whether decode has finished (so produceInto can distinguish "no more frames because
    // decode hasn't produced them yet" from "no more frames because playback genuinely ended").
    void setSourceComplete(bool complete) noexcept { m_sourceComplete = complete; }

    // The single reducer every transition goes through (M03: "so the UI, the debugger and tests
    // observe the same sequence"). Illegal transitions are logged and return an Error; the state
    // itself is left unchanged.
    Result<void> dispatch(const TransportEvent& event);

    // Producer-side pump: pulls from the source AudioBuffer, resamples, and writes resampled
    // frames into `ring`, stopping once `maxFrames` output frames have been written, the ring runs
    // out of write headroom, or playback can't currently advance (not yet decoded, or genuinely
    // ended). Returns the number of output frames actually written.
    std::size_t produceInto(RingBuffer& ring, std::size_t maxFrames);

    [[nodiscard]] const TransportState& state() const noexcept { return m_state; }
    [[nodiscard]] SampleRate            sourceRate() const noexcept { return m_sourceRate; }

    // Source frame index of the most recently *produced* (resampled-and-queued) sample — not the
    // audible position. The audible position additionally subtracts device output latency and
    // whatever the ring/render buffer still holds; that correction happens on the consumer side
    // (M03 "Position accuracy" — this is the frontend's positionClock.ts).
    [[nodiscard]] FrameIndex producedSourceFrame() const noexcept { return m_readCursor; }

private:
    static constexpr std::size_t kSourceBlockFrames = 1024;
    static constexpr std::size_t kScratchOutFrames  = 8192;  // headroom for upsampling ratios up to 8x

    Result<void> illegal(const char* eventName);
    void         handleLoopWrap();

    ChannelIndex m_channels;
    SampleRate   m_sourceRate;

    TransportState  m_state;
    TransportStatus m_resumeStatus = TransportStatus::Paused;  // status to restore once Seeking resolves

    const AudioBuffer* m_buffer         = nullptr;
    bool               m_sourceComplete = false;
    FrameIndex         m_readCursor     = 0;
    std::size_t        m_loopCrossfadeFrames = 0;
    bool               m_pendingFadeIn       = false;  // armed by SeekTo; consumed by the next produceInto()

    Resampler m_resampler;

    // Producer-thread scratch storage, sized once at construction — no allocation on the hot path.
    std::vector<std::vector<Sample>> m_scratchIn;   // [channel][kSourceBlockFrames]
    std::vector<std::vector<Sample>> m_scratchOut;  // [channel][kScratchOutFrames]
};

}  // namespace aud::playback

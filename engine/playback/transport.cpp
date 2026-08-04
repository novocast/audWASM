#include "transport.hpp"

#include <algorithm>

#include "../util/logging.hpp"
#include "fade.hpp"

namespace aud::playback {

namespace {
constexpr std::string_view kDomain = "playback.transport";
}

Transport::Transport(ChannelIndex channels, SampleRate sourceRate, SampleRate outputSampleRate,
                      Resampler::Quality quality)
    : m_channels(channels),
      m_sourceRate(sourceRate),
      m_resampler(sourceRate, outputSampleRate, channels, quality) {
    m_state.outputSampleRate = outputSampleRate;

    m_scratchIn.resize(channels);
    m_scratchOut.resize(channels);
    for (ChannelIndex ch = 0; ch < channels; ++ch) {
        m_scratchIn[ch].resize(kSourceBlockFrames);
        m_scratchOut[ch].resize(kScratchOutFrames);
    }
}

Result<void> Transport::illegal(const char* eventName) {
    AUD_LOG_WARN(kDomain, eventName);
    return Error{ErrorCode::InvalidArgument, kDomain, std::string("illegal transition: ") + eventName};
}

Result<void> Transport::dispatch(const TransportEvent& event) {
    using Kind   = TransportEventKind;
    using Status = TransportStatus;

    switch (event.kind) {
        case Kind::Load: {
            if (m_state.status != Status::Idle && m_state.status != Status::Ended) {
                return illegal("Load");
            }
            m_state.status         = Status::Loading;
            m_state.durationFrames = kNoFrame;
            m_readCursor            = 0;
            m_resampler.reset();
            return {};
        }
        case Kind::ReadySignal: {
            if (m_state.status != Status::Loading) {
                return illegal("ReadySignal");
            }
            m_state.status         = Status::Ready;
            m_state.durationFrames = event.frame;
            return {};
        }
        case Kind::Play: {
            if (m_state.status == Status::Ready || m_state.status == Status::Paused) {
                m_state.status = Status::Playing;
                return {};
            }
            if (m_state.status == Status::Playing) {
                return {};  // idempotent
            }
            return illegal("Play");
        }
        case Kind::Pause: {
            if (m_state.status == Status::Playing) {
                m_state.status = Status::Paused;
                return {};
            }
            if (m_state.status == Status::Paused) {
                return {};  // idempotent
            }
            return illegal("Pause");
        }
        case Kind::SeekTo: {
            const bool seekable = m_state.status == Status::Playing || m_state.status == Status::Paused ||
                                   m_state.status == Status::Ready || m_state.status == Status::Ended;
            if (!seekable) {
                return illegal("SeekTo");
            }
            m_resumeStatus  = (m_state.status == Status::Playing) ? Status::Playing : Status::Paused;
            m_state.status  = Status::Seeking;
            const FrameIndex maxFrame = m_state.durationFrames == kNoFrame ? 0 : m_state.durationFrames;
            m_readCursor    = std::clamp<FrameIndex>(event.frame, 0, std::max<FrameIndex>(0, maxFrame));
            m_resampler.reset();
            m_pendingFadeIn = true;  // M03 "Seeking" step 4: cosine fade-in on the refilled ring
            return {};
        }
        case Kind::SetLoopRange:
            m_state.loop.range = event.range;
            return {};
        case Kind::SetLoopEnabled:
            m_state.loop.enabled = event.flag;
            return {};
        case Kind::SetLoopCrossfadeFrames:
            m_loopCrossfadeFrames = event.frame > 0 ? static_cast<std::size_t>(event.frame) : 0;
            return {};
        case Kind::SetRate:
            if (event.number != 1.0) {
                return Error{ErrorCode::NotImplemented, kDomain,
                             "non-1.0 playback rate is out of v1 scope (M03 'Playback rate')"};
            }
            m_state.rate = event.number;
            return {};
        case Kind::SetGain:
            m_state.gain = event.gain;
            return {};
        case Kind::Reset: {
            const SampleRate outputRate = m_state.outputSampleRate;
            m_state                    = TransportState{};
            m_state.outputSampleRate   = outputRate;
            m_buffer                   = nullptr;
            m_sourceComplete           = false;
            m_readCursor                = 0;
            m_resampler.reset();
            return {};
        }
        case Kind::EndReached: {
            if (m_state.status != Status::Playing) {
                return illegal("EndReached");
            }
            m_state.status = Status::Ended;
            return {};
        }
    }
    return Error{ErrorCode::InvalidArgument, kDomain, "unknown TransportEvent kind"};
}

void Transport::handleLoopWrap() {
    // Exact-loop wrap: sample-accurate, no main-thread round trip (M03 "Looping"). A short
    // crossfade at the wrap point (`m_loopCrossfadeFrames`) is accepted as state but not yet
    // blended here — TODO(M03): blend the outgoing tail against the loop start before wrapping,
    // tracked alongside the rest of the click-free-everything work.
    m_readCursor = m_state.loop.range.begin;
}

std::size_t Transport::produceInto(RingBuffer& ring, std::size_t maxFrames) {
    if (m_buffer == nullptr) {
        return 0;
    }
    if (m_state.status != TransportStatus::Playing && m_state.status != TransportStatus::Seeking) {
        return 0;
    }

    std::size_t totalProduced = 0;

    while (totalProduced < maxFrames) {
        const std::size_t ringHeadroom = ring.framesAvailableToWrite();
        if (ringHeadroom == 0) {
            break;
        }

        const FrameIndex bufferFrames = m_buffer->frameCount();
        const bool       looping      = m_state.loop.enabled && m_state.loop.range.frameCount() > 0;
        const FrameIndex rangeEnd     = looping ? std::min(bufferFrames, m_state.loop.range.end) : bufferFrames;

        if (m_readCursor >= rangeEnd) {
            if (looping) {
                handleLoopWrap();
                continue;
            }
            if (m_sourceComplete) {
                const std::size_t drainMax = std::min(ringHeadroom, kScratchOutFrames);
                std::vector<std::span<Sample>> drainOut(m_channels);
                for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
                    drainOut[ch] = std::span<Sample>(m_scratchOut[ch].data(), drainMax);
                }
                const std::size_t drained =
                    m_resampler.drain(std::span<std::span<Sample>>(drainOut), drainMax);
                if (drained > 0) {
                    std::vector<std::span<const Sample>> writeSpans(m_channels);
                    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
                        writeSpans[ch] = std::span<const Sample>(m_scratchOut[ch].data(), drained);
                    }
                    ring.write(std::span<const std::span<const Sample>>(writeSpans), drained);
                    totalProduced += drained;
                }
                dispatch(TransportEvent::endReached());
            } else {
                m_state.status = TransportStatus::Loading;  // decode hasn't produced this region yet
            }
            break;
        }

        const std::size_t maxOut = std::min({ringHeadroom, kScratchOutFrames, maxFrames - totalProduced});

        // Identity (rate-matched) resampling is a straight copy that cannot buffer unconsumed
        // input across calls (see Resampler::process()'s bypass branch) — so in that mode we must
        // never feed more source frames than `maxOut` can absorb, or the excess would be silently
        // dropped rather than reread next call. The general resampling path *does* buffer any
        // unconsumed input in its history, so it can safely take a full block regardless of
        // `maxOut`; capping it too is harmless (just smaller batches), so we apply the same cap
        // unconditionally rather than branching on isIdentity() here.
        const std::size_t wantSource = std::min(
            {kSourceBlockFrames, static_cast<std::size_t>(rangeEnd - m_readCursor), maxOut});

        std::vector<std::span<const Sample>> planarInConst(m_channels);
        bool                                  readOk = true;
        for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
            std::span<Sample> dst(m_scratchIn[ch].data(), wantSource);
            auto              result = m_buffer->read(ch, FrameRange{m_readCursor, m_readCursor + static_cast<FrameIndex>(wantSource)}, dst);
            if (!result.has_value()) {
                readOk = false;
                break;
            }
            planarInConst[ch] = std::span<const Sample>(dst);
        }
        if (!readOk) {
            m_state.status = TransportStatus::Loading;
            break;
        }

        std::vector<std::span<Sample>> planarOut(m_channels);
        for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
            planarOut[ch] = std::span<Sample>(m_scratchOut[ch].data(), maxOut);
        }

        const std::size_t producedOut =
            m_resampler.process(std::span<const std::span<const Sample>>(planarInConst), wantSource,
                                 std::span<std::span<Sample>>(planarOut), maxOut);

        if (producedOut > 0 && m_pendingFadeIn) {
            const std::size_t fadeFrames = std::min(producedOut, kRenderQuantumFrames);
            for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
                applyCosineRamp(m_scratchOut[ch].data(), fadeFrames, 0.0f, 1.0f);
            }
            m_pendingFadeIn = false;
        }

        if (producedOut > 0) {
            std::vector<std::span<const Sample>> writeSpans(m_channels);
            for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
                writeSpans[ch] = std::span<const Sample>(m_scratchOut[ch].data(), producedOut);
            }
            ring.write(std::span<const std::span<const Sample>>(writeSpans), producedOut);
            totalProduced += producedOut;
        }

        m_readCursor += static_cast<FrameIndex>(wantSource);

        if (m_state.status == TransportStatus::Seeking) {
            m_state.status = m_resumeStatus;
        }

        if (producedOut == 0 && wantSource < kSourceBlockFrames) {
            // No forward progress possible this call (resampler still filling lookahead on a tiny
            // final block) — avoid spinning; caller will call again once more input/headroom exists.
            break;
        }
    }

    m_state.positionFrames = m_readCursor;
    return totalProduced;
}

}  // namespace aud::playback

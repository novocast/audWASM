#pragma once

// Kaiser-windowed polyphase (band-limited interpolation) resampler. See M03 "Sample-rate
// mismatch": AudioContext.sampleRate is whatever the OS device is and a file's native rate
// commonly differs from it; linear interpolation is rejected because this is an analysis tool and
// users listen for artefacts, so a proper windowed-sinc reconstruction filter is used instead.
//
// The filter is built once at construction as a table of `phaseCount` polyphase sub-filters, each
// `numTaps` long, sampling a Kaiser-windowed sinc lowpass at the cutoff needed to avoid aliasing
// (half the *lower* of the two rates). Runtime work per output sample is one dot product against
// the nearest phase — no per-sample transcendental calls, no allocation.
//
// Position accounting: `sourceFramesConsumed()` reports how many whole source frames have been
// consumed so far (as a fixed-point-tracked double, monotonic, never reset except by reset()).
// Per M03 "Position accuracy": every timestamp shown to the user is a source frame index, and the
// conversion to output frames happens exactly once, at the UI boundary — this class is where the
// authoritative source-frame count lives.

#include <cstddef>
#include <span>
#include <vector>

#include "../util/audio_types.hpp"

namespace aud::playback {

class Resampler {
public:
    enum class Quality { Fast, Good, Best };

    Resampler(SampleRate sourceRate, SampleRate targetRate, ChannelIndex channels,
              Quality quality = Quality::Good);

    [[nodiscard]] SampleRate   sourceRate() const noexcept { return m_sourceRate; }
    [[nodiscard]] SampleRate   targetRate() const noexcept { return m_targetRate; }
    [[nodiscard]] ChannelIndex channelCount() const noexcept { return m_channels; }
    [[nodiscard]] bool         isIdentity() const noexcept { return m_sourceRate == m_targetRate; }

    // Total whole source frames consumed since construction or the last reset(); this is the
    // authoritative position for the UI (M03 "Position accuracy" — track in source frames, not
    // output frames).
    [[nodiscard]] FrameIndex sourceFramesConsumed() const noexcept {
        return static_cast<FrameIndex>(m_sourcePos);
    }

    // Feeds `framesIn` planar input frames and writes as many resampled output frames as fit in
    // `planarOut` (capacity `maxFramesOut` per channel), returning the number of output frames
    // produced. Because the reconstruction kernel looks slightly ahead of the current read
    // position, a small tail of fed input is held back internally until enough lookahead has
    // arrived (or drain() is called at end-of-stream) — this is normal resampler latency, not lost
    // data.
    std::size_t process(std::span<const std::span<const Sample>> planarIn, std::size_t framesIn,
                         std::span<std::span<Sample>> planarOut, std::size_t maxFramesOut);

    // Flushes any input held back for lookahead, treating it as if followed by silence. Call once
    // at end-of-stream to emit the final partial-kernel tail.
    std::size_t drain(std::span<std::span<Sample>> planarOut, std::size_t maxFramesOut);

    // Clears all history and resets the source position to 0. Used on seek (M03: the resampler's
    // internal state must not leak audio across a seek discontinuity).
    void reset() noexcept;

private:
    struct ChannelHistory {
        std::vector<Sample> samples;  // absolute index `bufferBase + i`
    };

    void compactHistory();
    Sample                    convolve(const ChannelHistory& history, double sourcePos) const noexcept;

    SampleRate   m_sourceRate;
    SampleRate   m_targetRate;
    ChannelIndex m_channels;

    std::size_t m_numTaps    = 0;
    std::size_t m_phaseCount = 0;
    double      m_cutoff     = 0.5;  // cycles per source-sample

    // m_table[phase * m_numTaps + tap], normalised so each phase sums to unity (exact DC gain).
    std::vector<float> m_table;

    std::vector<ChannelHistory> m_history;
    std::size_t                 m_historyBase = 0;  // absolute source-frame index of history[0]
    std::size_t                 m_totalFed    = 0;   // absolute source frames ever appended
    double                      m_sourcePos   = 0.0;  // next read position, absolute source frames
    double                      m_step        = 1.0;  // source frames advanced per output frame
};

}  // namespace aud::playback

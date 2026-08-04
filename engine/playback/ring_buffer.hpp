#pragma once

// Lock-free single-producer/single-consumer ring buffer of planar float frames. This is the queue
// sitting between the producer (worker or main thread, pulling decoded frames out of the
// AudioBuffer and resampling them) and the consumer (the audio render thread / AudioWorklet). See
// M03 "Architecture": in the SharedArrayBuffer path this same class runs directly over SAB-backed
// storage (both sides see the same memory); in the postMessage fallback it runs main-thread-side
// only and its output is what gets transferred as Float32Arrays.
//
// No allocation on the hot path: all storage is sized once at construction (M00 §5 thread-ready
// rule: "no allocation in the audio path"). Capacity is rounded up to a power of two so the index
// arithmetic is a mask instead of a modulo.
//
// The read/write cursors are monotonically increasing counters (never wrapped themselves — only
// the storage index derived from them is wrapped via the mask). This is the standard SPSC trick:
// `writeIndex - readIndex` (unsigned subtraction, wraps correctly modulo 2^64) always yields the
// true number of frames currently buffered, with no ambiguity between "empty" and "full".

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

#include "../util/audio_types.hpp"

namespace aud::playback {

class RingBuffer {
public:
    // `capacityFrames` is a minimum; actual capacity is rounded up to the next power of two.
    RingBuffer(ChannelIndex channels, std::size_t capacityFrames)
        : m_channels(channels), m_capacity(roundUpToPowerOfTwo(capacityFrames)), m_mask(m_capacity - 1) {
        m_data.assign(static_cast<std::size_t>(m_channels) * m_capacity, Sample{0});
    }

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    [[nodiscard]] std::size_t   capacityFrames() const noexcept { return m_capacity; }
    [[nodiscard]] ChannelIndex  channelCount() const noexcept { return m_channels; }

    // Safe to call from either side; each side only needs an approximate, monotonically-consistent
    // view of the other's cursor (acquire on the cursor the caller doesn't own).
    [[nodiscard]] std::size_t framesAvailableToRead() const noexcept {
        const std::size_t w = m_writeIndex.load(std::memory_order_acquire);
        const std::size_t r = m_readIndex.load(std::memory_order_relaxed);
        return w - r;
    }
    [[nodiscard]] std::size_t framesAvailableToWrite() const noexcept {
        const std::size_t w = m_writeIndex.load(std::memory_order_relaxed);
        const std::size_t r = m_readIndex.load(std::memory_order_acquire);
        return m_capacity - (w - r);
    }

    // Producer side (single writer thread only). `planarIn[ch]` must provide at least `frames`
    // samples for every channel in `[0, channelCount())`. Returns the number of frames actually
    // written, which is `min(frames, framesAvailableToWrite())` — callers must handle a short write
    // (the ring is a fixed-size backpressure point, not an unbounded queue).
    std::size_t write(std::span<const std::span<const Sample>> planarIn, std::size_t frames) noexcept {
        const std::size_t toWrite = std::min(frames, framesAvailableToWrite());
        const std::size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
            Sample*       dst = channelBase(ch);
            const Sample* src = planarIn[ch].data();
            for (std::size_t i = 0; i < toWrite; ++i) {
                dst[(writeIdx + i) & m_mask] = src[i];
            }
        }
        m_writeIndex.store(writeIdx + toWrite, std::memory_order_release);
        return toWrite;
    }

    // Consumer side (single reader thread only — the audio render thread). Returns the number of
    // frames actually read, which is `min(frames, framesAvailableToRead())`; the caller is
    // responsible for filling any shortfall with silence and counting it as a dropout (M03 risk
    // table: "Worklet starvation under main-thread load").
    std::size_t read(std::span<std::span<Sample>> planarOut, std::size_t frames) noexcept {
        const std::size_t toRead  = std::min(frames, framesAvailableToRead());
        const std::size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
            const Sample* src = channelBase(ch);
            Sample*       dst = planarOut[ch].data();
            for (std::size_t i = 0; i < toRead; ++i) {
                dst[i] = src[(readIdx + i) & m_mask];
            }
        }
        m_readIndex.store(readIdx + toRead, std::memory_order_release);
        return toRead;
    }

    // Drops all buffered content. Only safe when the producer is quiesced (Transport pauses
    // production before calling this — see M03 "Seeking" step 2: "signal the worklet to drop its
    // buffered content"). Not safe to call concurrently with write().
    void reset() noexcept {
        m_readIndex.store(0, std::memory_order_relaxed);
        m_writeIndex.store(0, std::memory_order_release);
    }

private:
    [[nodiscard]] Sample* channelBase(ChannelIndex ch) noexcept {
        return m_data.data() + static_cast<std::size_t>(ch) * m_capacity;
    }
    [[nodiscard]] const Sample* channelBase(ChannelIndex ch) const noexcept {
        return m_data.data() + static_cast<std::size_t>(ch) * m_capacity;
    }

    static std::size_t roundUpToPowerOfTwo(std::size_t v) noexcept {
        std::size_t p = 1;
        while (p < v) {
            p <<= 1;
        }
        return p;
    }

    ChannelIndex m_channels;
    std::size_t  m_capacity;
    std::size_t  m_mask;

    std::vector<Sample> m_data;

    // Cache-line separated so producer and consumer writes to their own cursor don't false-share.
    alignas(64) std::atomic<std::size_t> m_writeIndex{0};
    alignas(64) std::atomic<std::size_t> m_readIndex{0};
};

}  // namespace aud::playback

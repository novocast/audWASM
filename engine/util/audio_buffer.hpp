#pragma once

// Chunked, per-channel-planar PCM storage. See M00 §3 for the full rationale: chunking lets us
// grow during progressive decode without a monolithic reallocation/copy, keeps every single
// allocation bounded by kChunkFrames * sizeof(Sample) regardless of file length, and lets analysis
// code iterate without ever needing "a pointer to the whole channel".

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "allocator.hpp"
#include "audio_types.hpp"
#include "result.hpp"

namespace aud {

class AudioBuffer {
public:
    static constexpr std::size_t kChunkFrames = 1u << 16;  // 65536 frames ~= 1.49s @ 44.1kHz

    AudioBuffer(const AudioBuffer&)            = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    AudioBuffer(AudioBuffer&&) noexcept;
    AudioBuffer& operator=(AudioBuffer&&) noexcept;
    ~AudioBuffer();

    // The only way to construct one: validates arguments, never allocates any chunk storage
    // up front (chunks materialise lazily as frames are appended).
    static Result<AudioBuffer> create(SampleRate sampleRate, ChannelIndex channels,
                                       Allocator& allocator = defaultAllocator());

    [[nodiscard]] SampleRate   sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] ChannelIndex channelCount() const noexcept { return m_channels; }
    [[nodiscard]] FrameIndex   frameCount() const noexcept { return m_frameCount; }
    [[nodiscard]] double       durationSeconds() const noexcept {
        return m_sampleRate == 0 ? 0.0 : static_cast<double>(m_frameCount) / static_cast<double>(m_sampleRate);
    }

    [[nodiscard]] std::size_t chunkCount() const noexcept;

    // Contiguous access to one chunk of one channel. Never spans a chunk boundary. The returned
    // span covers only the frames actually written into that chunk so far.
    [[nodiscard]] std::span<const Sample> chunk(ChannelIndex ch, std::size_t chunkIndex) const noexcept;

    // Convenience copy-out for callers that need an arbitrary contiguous range; stitches across
    // chunk boundaries internally.
    [[nodiscard]] Result<void> read(ChannelIndex ch, FrameRange range, std::span<Sample> out) const;

    // Appends `frames` worth of planar samples (one span per channel, each of length `frames`) to
    // the end of the buffer, growing chunk storage as needed. Used by the decode pipeline (M02);
    // analysis code never calls this.
    [[nodiscard]] Result<void> append(std::span<const std::span<const Sample>> planarChannels,
                                       std::size_t frames);

private:
    AudioBuffer(SampleRate sampleRate, ChannelIndex channels, Allocator& allocator);

    struct ChunkStorage {
        Sample*     data      = nullptr;
        std::size_t framesUsed = 0;
    };

    [[nodiscard]] Result<void> ensureChunkForWrite(std::size_t chunkIndex);
    void                       releaseAll() noexcept;

    SampleRate   m_sampleRate = 0;
    ChannelIndex m_channels   = 0;
    FrameIndex   m_frameCount = 0;
    Allocator*   m_allocator  = nullptr;

    // Outer: one vector of chunks per channel. Inner: chunks in order. All channels always have
    // the same chunk count (kept in lockstep by append()).
    std::vector<std::vector<ChunkStorage>> m_channelChunks;
};

}  // namespace aud

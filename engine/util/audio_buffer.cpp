#include "audio_buffer.hpp"

#include <algorithm>
#include <cstring>

#include "assert.hpp"

namespace aud {

AudioBuffer::AudioBuffer(SampleRate sampleRate, ChannelIndex channels, Allocator& allocator)
    : m_sampleRate(sampleRate), m_channels(channels), m_allocator(&allocator), m_channelChunks(channels) {}

AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : m_sampleRate(other.m_sampleRate),
      m_channels(other.m_channels),
      m_frameCount(other.m_frameCount),
      m_allocator(other.m_allocator),
      m_channelChunks(std::move(other.m_channelChunks)) {
    other.m_frameCount = 0;
    other.m_channelChunks.clear();
}

AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept {
    if (this != &other) {
        releaseAll();
        m_sampleRate    = other.m_sampleRate;
        m_channels      = other.m_channels;
        m_frameCount    = other.m_frameCount;
        m_allocator     = other.m_allocator;
        m_channelChunks = std::move(other.m_channelChunks);
        other.m_frameCount = 0;
        other.m_channelChunks.clear();
    }
    return *this;
}

AudioBuffer::~AudioBuffer() { releaseAll(); }

void AudioBuffer::releaseAll() noexcept {
    if (m_allocator == nullptr) {
        return;
    }
    for (auto& chunks : m_channelChunks) {
        for (auto& chunk : chunks) {
            if (chunk.data != nullptr) {
                m_allocator->deallocate(chunk.data, kChunkFrames * sizeof(Sample), alignof(Sample));
            }
        }
    }
    m_channelChunks.clear();
}

Result<AudioBuffer> AudioBuffer::create(SampleRate sampleRate, ChannelIndex channels, Allocator& allocator) {
    if (sampleRate == 0 || channels == 0) {
        return Error{ErrorCode::InvalidArgument, "util.audio_buffer", "sampleRate and channels must be > 0"};
    }
    return AudioBuffer(sampleRate, channels, allocator);
}

std::size_t AudioBuffer::chunkCount() const noexcept {
    return m_channelChunks.empty() ? 0 : m_channelChunks[0].size();
}

std::span<const Sample> AudioBuffer::chunk(ChannelIndex ch, std::size_t chunkIndex) const noexcept {
    if (ch >= m_channels || chunkIndex >= chunkCount()) {
        return {};
    }
    const ChunkStorage& storage = m_channelChunks[ch][chunkIndex];
    return std::span<const Sample>(storage.data, storage.framesUsed);
}

Result<void> AudioBuffer::ensureChunkForWrite(std::size_t chunkIndex) {
    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        auto& chunks = m_channelChunks[ch];
        while (chunks.size() <= chunkIndex) {
            AUD_TRY_ASSIGN(raw, tryAllocate(*m_allocator, kChunkFrames * sizeof(Sample), alignof(Sample)));
            chunks.push_back(ChunkStorage{static_cast<Sample*>(raw), 0});
        }
    }
    return Result<void>{};
}

Result<void> AudioBuffer::append(std::span<const std::span<const Sample>> planarChannels, std::size_t frames) {
    if (planarChannels.size() != m_channels) {
        return Error{ErrorCode::InvalidArgument, "util.audio_buffer", "channel count mismatch"};
    }
    for (const auto& channelSpan : planarChannels) {
        if (channelSpan.size() < frames) {
            return Error{ErrorCode::InvalidArgument, "util.audio_buffer", "source span shorter than frame count"};
        }
    }

    std::size_t written = 0;
    while (written < frames) {
        const auto        currentFrame = static_cast<std::size_t>(m_frameCount);
        const std::size_t chunkIndex   = currentFrame / kChunkFrames;
        const std::size_t offsetInChunk = currentFrame % kChunkFrames;

        AUD_TRY(ensureChunkForWrite(chunkIndex));

        const std::size_t roomInChunk = kChunkFrames - offsetInChunk;
        const std::size_t toWrite     = std::min(roomInChunk, frames - written);

        for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
            ChunkStorage& storage = m_channelChunks[ch][chunkIndex];
            std::memcpy(storage.data + offsetInChunk, planarChannels[ch].data() + written,
                        toWrite * sizeof(Sample));
            storage.framesUsed = std::max(storage.framesUsed, offsetInChunk + toWrite);
        }

        written += toWrite;
        m_frameCount += static_cast<FrameIndex>(toWrite);
    }

    return Result<void>{};
}

Result<void> AudioBuffer::read(ChannelIndex ch, FrameRange range, std::span<Sample> out) const {
    if (ch >= m_channels) {
        return Error{ErrorCode::InvalidArgument, "util.audio_buffer", "channel index out of range"};
    }
    if (range.begin < 0 || range.end > m_frameCount || range.empty()) {
        return Error{ErrorCode::InvalidArgument, "util.audio_buffer", "frame range out of bounds"};
    }
    const auto frameCountRequested = static_cast<std::size_t>(range.frameCount());
    if (out.size() < frameCountRequested) {
        return Error{ErrorCode::InvalidArgument, "util.audio_buffer", "output span too small"};
    }

    std::size_t copied = 0;
    auto        frame   = static_cast<std::size_t>(range.begin);
    while (copied < frameCountRequested) {
        const std::size_t chunkIndex    = frame / kChunkFrames;
        const std::size_t offsetInChunk = frame % kChunkFrames;
        const auto&        storage      = m_channelChunks[ch][chunkIndex];

        const std::size_t availableInChunk = storage.framesUsed - offsetInChunk;
        const std::size_t toCopy = std::min(availableInChunk, frameCountRequested - copied);

        std::memcpy(out.data() + copied, storage.data + offsetInChunk, toCopy * sizeof(Sample));

        copied += toCopy;
        frame += toCopy;
    }

    return Result<void>{};
}

}  // namespace aud

#pragma once

// Sequential and windowed access over an AudioBuffer channel that hides kChunkFrames boundary
// arithmetic from analysis code. See M00 §3: "Analysis code must be written against a chunk
// iterator or the read() copy-out, never against 'give me a pointer to the whole channel'."

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include "audio_buffer.hpp"
#include "audio_types.hpp"

namespace aud {

// Sequential forward reader. Cheap to construct; holds no allocation of its own.
class ChunkReader {
public:
    ChunkReader(const AudioBuffer& buffer, ChannelIndex channel) noexcept
        : m_buffer(&buffer), m_channel(channel) {}

    [[nodiscard]] FrameIndex position() const noexcept { return m_position; }
    [[nodiscard]] bool       atEnd() const noexcept { return m_position >= m_buffer->frameCount(); }

    void seek(FrameIndex frame) noexcept { m_position = frame; }

    // Copies up to out.size() frames starting at the current position, advancing it. Returns the
    // number of frames actually copied (less than out.size() only at end of buffer).
    std::size_t read(std::span<Sample> out) {
        const FrameIndex remaining = m_buffer->frameCount() - m_position;
        if (remaining <= 0) {
            return 0;
        }
        const auto toRead = static_cast<std::size_t>(std::min<FrameIndex>(remaining, static_cast<FrameIndex>(out.size())));
        if (toRead == 0) {
            return 0;
        }
        auto result = m_buffer->read(m_channel, FrameRange{m_position, m_position + static_cast<FrameIndex>(toRead)},
                                      out.first(toRead));
        if (!result.has_value()) {
            return 0;  // programmer error (bad range) already impossible given the clamp above
        }
        m_position += static_cast<FrameIndex>(toRead);
        return toRead;
    }

private:
    const AudioBuffer* m_buffer;
    ChannelIndex        m_channel;
    FrameIndex          m_position = 0;
};

// Calls fn(window, windowStartFrame) for each fully-populated window of `size` frames, hopping by
// `hop` frames, from frame 0 until a window would run past the end of the buffer. Windows that
// happen to fall within a single chunk are handed to fn as a direct view (no copy); windows that
// straddle a chunk boundary are stitched into `scratch` first. `scratch` must have capacity for at
// least `size` frames and is reused across calls, so forEachWindow itself never allocates.
template <class Fn>
void forEachWindow(const AudioBuffer& buffer, ChannelIndex channel, std::size_t hop, std::size_t size,
                    std::vector<Sample>& scratch, Fn&& fn) {
    if (hop == 0 || size == 0) {
        return;
    }
    scratch.resize(size);

    const auto frameCount = static_cast<std::size_t>(std::max<FrameIndex>(buffer.frameCount(), 0));
    if (size > frameCount) {
        return;
    }

    for (std::size_t start = 0; start + size <= frameCount; start += hop) {
        const std::size_t chunkFrames    = AudioBuffer::kChunkFrames;
        const std::size_t startChunk     = start / chunkFrames;
        const std::size_t endChunk       = (start + size - 1) / chunkFrames;

        if (startChunk == endChunk) {
            const std::size_t offsetInChunk = start % chunkFrames;
            std::span<const Sample> chunkSpan = buffer.chunk(channel, startChunk);
            fn(chunkSpan.subspan(offsetInChunk, size), static_cast<FrameIndex>(start));
        } else {
            const FrameRange range{static_cast<FrameIndex>(start), static_cast<FrameIndex>(start + size)};
            auto              result = buffer.read(channel, range, std::span<Sample>(scratch.data(), size));
            if (!result.has_value()) {
                return;
            }
            fn(std::span<const Sample>(scratch.data(), size), static_cast<FrameIndex>(start));
        }
    }
}

}  // namespace aud

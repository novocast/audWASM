#include "decode_session.hpp"

namespace aud::decoder {

Result<DecodeSession> DecodeSession::create(std::span<const std::byte> probeBytes, Allocator& allocator) {
    AUD_TRY_ASSIGN(decoderPtr, createDecoder(probeBytes));
    return DecodeSession(std::move(decoderPtr), allocator);
}

Result<void> DecodeSession::ensureBufferCreated() {
    if (m_buffer.has_value()) {
        return Result<void>{};
    }
    auto infoResult = m_decoder->info();
    if (!infoResult.has_value()) {
        return Result<void>{};  // decoder not initialized yet; try again after the next feed()
    }
    const StreamInfo& info = infoResult.value();
    AUD_TRY_ASSIGN(created, AudioBuffer::create(info.sampleRate, info.channels, *m_allocator));
    m_buffer.emplace(std::move(created));
    return Result<void>{};
}

Result<void> DecodeSession::drainAvailableFrames() {
    AUD_TRY(ensureBufferCreated());
    if (!m_buffer.has_value()) {
        return Result<void>{};
    }

    const ChannelIndex channels = m_buffer->channelCount();
    std::vector<std::vector<Sample>> scratchStorage(channels, std::vector<Sample>(kReadChunkFrames));
    std::vector<std::span<Sample>>   planarOut(channels);
    for (ChannelIndex ch = 0; ch < channels; ++ch) {
        planarOut[ch] = std::span<Sample>(scratchStorage[ch]);
    }

    bool anyDecoded = false;
    for (;;) {
        AUD_TRY_ASSIGN(framesRead, m_decoder->read(planarOut));
        if (framesRead == 0) {
            break;
        }
        anyDecoded = true;

        std::vector<std::span<const Sample>> constPlanar(channels);
        for (ChannelIndex ch = 0; ch < channels; ++ch) {
            constPlanar[ch] = std::span<const Sample>(scratchStorage[ch].data(), framesRead);
        }
        AUD_TRY(m_buffer->append(constPlanar, framesRead));
    }

    if (anyDecoded && m_progress) {
        auto infoResult = m_decoder->info();
        DecodeProgress progress;
        progress.framesDecoded = m_buffer->frameCount();
        progress.seconds       = m_buffer->durationSeconds();
        if (infoResult.has_value()) {
            progress.estimatedTotal = infoResult.value().frameCount;
            progress.isEstimate     = infoResult.value().isEstimate || infoResult.value().frameCount == kNoFrame;
        }
        m_progress(progress);
    }

    return Result<void>{};
}

Result<void> DecodeSession::feed(std::span<const std::byte> bytes) {
    AUD_TRY(m_decoder->feed(bytes));
    return drainAvailableFrames();
}

Result<void> DecodeSession::finish() {
    AUD_TRY(m_decoder->signalEndOfInput());
    return drainAvailableFrames();
}

Result<StreamInfo> DecodeSession::streamInfo() const { return m_decoder->info(); }

}  // namespace aud::decoder

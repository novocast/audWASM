#include "mp3_decoder.hpp"

#include <vector>

#include "dr_mp3.h"

namespace aud::decoder {

namespace {

constexpr std::size_t kMaxHeaderProbeBytes = 1u << 20;

std::size_t onRead(void* userData, void* out, std::size_t bytesToRead) {
    return static_cast<ByteRing*>(userData)->readInto(out, bytesToRead);
}

drmp3_bool32 onSeek(void* userData, int offset, drmp3_seek_origin origin) {
    auto* ring = static_cast<ByteRing*>(userData);
    const std::size_t base   = (origin == DRMP3_SEEK_SET) ? 0 : ring->cursor();
    const auto         target = static_cast<std::int64_t>(base) + offset;
    if (target < 0) {
        return DRMP3_FALSE;
    }
    return ring->seek(static_cast<std::size_t>(target)) ? DRMP3_TRUE : DRMP3_FALSE;
}

}  // namespace

struct Mp3Decoder::Impl {
    drmp3 mp3{};
};

Mp3Decoder::Mp3Decoder() : m_impl(std::make_unique<Impl>()) {}

Mp3Decoder::~Mp3Decoder() {
    if (m_initialized) {
        drmp3_uninit(&m_impl->mp3);
    }
}

Result<void> Mp3Decoder::tryInit() {
    m_ring.seek(0);
    if (drmp3_init(&m_impl->mp3, onRead, onSeek, nullptr, nullptr, &m_ring, nullptr)) {
        m_initialized = true;
        return Result<void>{};
    }
    if (m_ring.totalBytes() > kMaxHeaderProbeBytes) {
        return Error{ErrorCode::CorruptData, "decoder.mp3", "could not find a valid MP3 frame sync"};
    }
    return Result<void>{};
}

Result<void> Mp3Decoder::feed(std::span<const std::byte> bytes) {
    m_ring.feed(bytes.data(), bytes.size());
    if (!m_initialized) {
        return tryInit();
    }
    return Result<void>{};
}

Result<void> Mp3Decoder::signalEndOfInput() {
    m_endOfInput = true;
    if (!m_initialized) {
        AUD_TRY(tryInit());
        if (!m_initialized) {
            return Error{ErrorCode::TruncatedData, "decoder.mp3", "end of input before a valid frame sync was found"};
        }
    }
    return Result<void>{};
}

Result<StreamInfo> Mp3Decoder::info() const {
    if (!m_initialized) {
        return Error{ErrorCode::InvalidArgument, "decoder.mp3", "not yet initialized"};
    }
    StreamInfo si;
    si.sampleRate = m_impl->mp3.sampleRate;
    si.channels   = m_impl->mp3.channels;
    si.frameCount = kNoFrame;  // TODO(M02): Xing/LAME/VBRI frame-count + delay/padding parsing
    si.codecName  = "mp3";
    si.bitDepth   = 0;
    si.isLossy    = true;
    si.isEstimate = true;
    return si;
}

Result<std::size_t> Mp3Decoder::read(std::span<std::span<Sample>> planarOut) {
    if (!m_initialized) {
        return std::size_t{0};
    }
    if (planarOut.empty()) {
        return Error{ErrorCode::InvalidArgument, "decoder.mp3", "no output channels provided"};
    }
    const ChannelIndex channels = m_impl->mp3.channels;
    if (planarOut.size() != channels) {
        return Error{ErrorCode::InvalidArgument, "decoder.mp3", "output channel count mismatch"};
    }

    const std::size_t  framesRequested = planarOut[0].size();
    std::vector<float> interleaved(framesRequested * channels);

    const drmp3_uint64 framesRead = drmp3_read_pcm_frames_f32(&m_impl->mp3, framesRequested, interleaved.data());

    for (std::size_t frame = 0; frame < framesRead; ++frame) {
        for (ChannelIndex ch = 0; ch < channels; ++ch) {
            planarOut[ch][frame] = interleaved[(frame * channels) + ch];
        }
    }

    return static_cast<std::size_t>(framesRead);
}

}  // namespace aud::decoder

#include "mp3_decoder.hpp"

#include <vector>

#include "dr_mp3.h"

namespace aud::decoder {

namespace {

// Bounds how many bytes drmp3_init() (which skips any leading ID3v2 tag internally) will scan
// before giving up. Was 1MB; raised to 16MB after a real-world failure: ID3v2 tags carrying
// embedded high-resolution cover art routinely run into several MB, and drmp3_init needs to see
// past the *entire* tag before it can find the first real frame sync. Buffering that much of the
// (still-encoded, not yet decoded) byte stream is cheap relative to the AudioBuffer chunking
// budget — see byte_ring.hpp's identical rationale.
constexpr std::size_t kMaxHeaderProbeBytes = 16u << 20;

std::size_t onRead(void* userData, void* out, std::size_t bytesToRead) {
    return static_cast<ByteRing*>(userData)->readInto(out, bytesToRead);
}

drmp3_bool32 onSeek(void* userData, int offset, drmp3_seek_origin origin) {
    auto* ring = static_cast<ByteRing*>(userData);
    // DRMP3_SEEK_END (used for trailing tag / stream-length detection) must not fall through to
    // the DRMP3_SEEK_CUR case below — see the identical bug fixed in wav_decoder.cpp's onSeek.
    const std::size_t base   = origin == DRMP3_SEEK_SET   ? 0
                                : origin == DRMP3_SEEK_END ? ring->totalBytes()
                                                            : ring->cursor();
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
        m_initialized         = true;
        m_lastRingBytesAtInit = m_ring.totalBytes();
        return Result<void>{};
    }
    if (m_ring.totalBytes() > kMaxHeaderProbeBytes) {
        return Error{ErrorCode::CorruptData, "decoder.mp3", "could not find a valid MP3 frame sync"};
    }
    return Result<void>{};
}

Result<void> Mp3Decoder::refreshIfGrown() {
    if (m_ring.totalBytes() == m_lastRingBytesAtInit) {
        return Result<void>{};  // nothing new since the last (re-)init
    }
    drmp3_uninit(&m_impl->mp3);
    m_initialized = false;
    AUD_TRY(tryInit());
    if (m_initialized && m_framesDelivered > 0) {
        drmp3_seek_to_pcm_frame(&m_impl->mp3, m_framesDelivered);
    }
    return Result<void>{};
}

Result<void> Mp3Decoder::feed(std::span<const std::byte> bytes) {
    m_ring.feed(bytes.data(), bytes.size());
    if (!m_initialized) {
        return tryInit();
    }
    return refreshIfGrown();
}

Result<void> Mp3Decoder::signalEndOfInput() {
    m_endOfInput = true;
    if (!m_initialized) {
        AUD_TRY(tryInit());
        if (!m_initialized) {
            return Error{ErrorCode::TruncatedData, "decoder.mp3", "end of input before a valid frame sync was found"};
        }
        return Result<void>{};
    }
    return refreshIfGrown();
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

    m_framesDelivered += static_cast<std::uint64_t>(framesRead);
    return static_cast<std::size_t>(framesRead);
}

}  // namespace aud::decoder

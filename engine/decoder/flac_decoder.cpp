#include "flac_decoder.hpp"

#include <vector>

#include "dr_flac.h"

namespace aud::decoder {

namespace {

constexpr std::size_t kMaxHeaderProbeBytes = 1u << 20;

std::size_t onRead(void* userData, void* out, std::size_t bytesToRead) {
    return static_cast<ByteRing*>(userData)->readInto(out, bytesToRead);
}

drflac_bool32 onSeek(void* userData, int offset, drflac_seek_origin origin) {
    auto* ring = static_cast<ByteRing*>(userData);
    // DRFLAC_SEEK_END (used for end-of-stream metadata/length detection) must not fall through to
    // the DRFLAC_SEEK_CUR case below — see the identical bug fixed in wav_decoder.cpp's onSeek.
    const std::size_t base   = origin == DRFLAC_SEEK_SET   ? 0
                                : origin == DRFLAC_SEEK_END ? ring->totalBytes()
                                                             : ring->cursor();
    const auto         target = static_cast<std::int64_t>(base) + offset;
    if (target < 0) {
        return DRFLAC_FALSE;
    }
    return ring->seek(static_cast<std::size_t>(target)) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

drflac_bool32 onTell(void* userData, drflac_int64* pCursor) {
    *pCursor = static_cast<drflac_int64>(static_cast<ByteRing*>(userData)->cursor());
    return DRFLAC_TRUE;
}

}  // namespace

struct FlacDecoder::Impl {
    drflac* flac = nullptr;
};

FlacDecoder::FlacDecoder() : m_impl(std::make_unique<Impl>()) {}

FlacDecoder::~FlacDecoder() {
    if (m_impl->flac != nullptr) {
        drflac_close(m_impl->flac);
    }
}

Result<void> FlacDecoder::tryInit() {
    m_ring.seek(0);
    m_impl->flac = drflac_open(onRead, onSeek, onTell, &m_ring, nullptr);
    if (m_impl->flac != nullptr) {
        m_initialized = true;
        return Result<void>{};
    }
    if (m_ring.totalBytes() > kMaxHeaderProbeBytes) {
        return Error{ErrorCode::CorruptData, "decoder.flac", "could not parse FLAC header"};
    }
    return Result<void>{};
}

Result<void> FlacDecoder::feed(std::span<const std::byte> bytes) {
    m_ring.feed(bytes.data(), bytes.size());
    if (!m_initialized) {
        return tryInit();
    }
    return Result<void>{};
}

Result<void> FlacDecoder::signalEndOfInput() {
    m_endOfInput = true;
    if (!m_initialized) {
        AUD_TRY(tryInit());
        if (!m_initialized) {
            return Error{ErrorCode::TruncatedData, "decoder.flac", "end of input before a valid header was parsed"};
        }
    }
    return Result<void>{};
}

Result<StreamInfo> FlacDecoder::info() const {
    if (!m_initialized) {
        return Error{ErrorCode::InvalidArgument, "decoder.flac", "not yet initialized"};
    }
    StreamInfo si;
    si.sampleRate = m_impl->flac->sampleRate;
    si.channels   = m_impl->flac->channels;
    si.frameCount = m_impl->flac->totalPCMFrameCount > 0
                        ? static_cast<FrameIndex>(m_impl->flac->totalPCMFrameCount)
                        : kNoFrame;
    si.codecName  = "flac";
    si.bitDepth   = m_impl->flac->bitsPerSample;
    si.isLossy    = false;
    return si;
}

Result<std::size_t> FlacDecoder::read(std::span<std::span<Sample>> planarOut) {
    if (!m_initialized) {
        return std::size_t{0};
    }
    if (planarOut.empty()) {
        return Error{ErrorCode::InvalidArgument, "decoder.flac", "no output channels provided"};
    }
    const ChannelIndex channels = m_impl->flac->channels;
    if (planarOut.size() != channels) {
        return Error{ErrorCode::InvalidArgument, "decoder.flac", "output channel count mismatch"};
    }

    const std::size_t  framesRequested = planarOut[0].size();
    std::vector<float> interleaved(framesRequested * channels);

    const drflac_uint64 framesRead = drflac_read_pcm_frames_f32(m_impl->flac, framesRequested, interleaved.data());

    for (std::size_t frame = 0; frame < framesRead; ++frame) {
        for (ChannelIndex ch = 0; ch < channels; ++ch) {
            planarOut[ch][frame] = interleaved[(frame * channels) + ch];
        }
    }

    return static_cast<std::size_t>(framesRead);
}

}  // namespace aud::decoder

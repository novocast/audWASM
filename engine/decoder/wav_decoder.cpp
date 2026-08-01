#include "wav_decoder.hpp"

#include <algorithm>
#include <vector>

#include "dr_wav.h"

namespace aud::decoder {

namespace {

// A header that hasn't parsed successfully after this many buffered bytes is treated as genuinely
// corrupt rather than "just needs more data" — WAV/AIFF fmt chunks are always near the start of the
// file, so this is generous.
constexpr std::size_t kMaxHeaderProbeBytes = 1u << 20;

std::size_t onRead(void* userData, void* out, std::size_t bytesToRead) {
    return static_cast<ByteRing*>(userData)->readInto(out, bytesToRead);
}

drwav_bool32 onSeek(void* userData, int offset, drwav_seek_origin origin) {
    auto* ring = static_cast<ByteRing*>(userData);
    const std::size_t base = (origin == DRWAV_SEEK_SET) ? 0 : ring->cursor();
    const auto         target = static_cast<std::int64_t>(base) + offset;
    if (target < 0) {
        return DRWAV_FALSE;
    }
    return ring->seek(static_cast<std::size_t>(target)) ? DRWAV_TRUE : DRWAV_FALSE;
}

drwav_bool32 onTell(void* userData, drwav_int64* cursor) {
    *cursor = static_cast<drwav_int64>(static_cast<ByteRing*>(userData)->cursor());
    return DRWAV_TRUE;
}

}  // namespace

struct WavDecoder::Impl {
    drwav wav{};
};

WavDecoder::WavDecoder() : m_impl(std::make_unique<Impl>()) {}

WavDecoder::~WavDecoder() {
    if (m_initialized) {
        drwav_uninit(&m_impl->wav);
    }
}

Result<void> WavDecoder::tryInit() {
    m_ring.seek(0);
    if (drwav_init(&m_impl->wav, onRead, onSeek, onTell, &m_ring, nullptr)) {
        m_initialized = true;
        return Result<void>{};
    }
    ++m_initAttempts;
    if (m_ring.totalBytes() > kMaxHeaderProbeBytes) {
        return Error{ErrorCode::CorruptData, "decoder.wav", "could not parse WAV/AIFF header"};
    }
    return Result<void>{};  // not enough data yet — try again on the next feed()
}

Result<void> WavDecoder::feed(std::span<const std::byte> bytes) {
    m_ring.feed(bytes.data(), bytes.size());
    if (!m_initialized) {
        return tryInit();
    }
    return Result<void>{};
}

Result<void> WavDecoder::signalEndOfInput() {
    m_endOfInput = true;
    if (!m_initialized) {
        AUD_TRY(tryInit());
        if (!m_initialized) {
            return Error{ErrorCode::TruncatedData, "decoder.wav", "end of input before a valid header was parsed"};
        }
    }
    return Result<void>{};
}

Result<StreamInfo> WavDecoder::info() const {
    if (!m_initialized) {
        return Error{ErrorCode::InvalidArgument, "decoder.wav", "not yet initialized"};
    }
    StreamInfo si;
    si.sampleRate  = m_impl->wav.sampleRate;
    si.channels    = m_impl->wav.channels;
    si.frameCount  = m_impl->wav.totalPCMFrameCount > 0
                          ? static_cast<FrameIndex>(m_impl->wav.totalPCMFrameCount)
                          : kNoFrame;
    si.codecName   = "wav";
    si.bitDepth    = m_impl->wav.bitsPerSample;
    si.isLossy     = false;
    return si;
}

Result<std::size_t> WavDecoder::read(std::span<std::span<Sample>> planarOut) {
    if (!m_initialized) {
        return std::size_t{0};
    }
    if (planarOut.empty()) {
        return Error{ErrorCode::InvalidArgument, "decoder.wav", "no output channels provided"};
    }
    const ChannelIndex channels = m_impl->wav.channels;
    if (planarOut.size() != channels) {
        return Error{ErrorCode::InvalidArgument, "decoder.wav", "output channel count mismatch"};
    }

    const std::size_t framesRequested = planarOut[0].size();
    std::vector<float> interleaved(framesRequested * channels);

    const drwav_uint64 framesRead = drwav_read_pcm_frames_f32(&m_impl->wav, framesRequested, interleaved.data());

    for (std::size_t frame = 0; frame < framesRead; ++frame) {
        for (ChannelIndex ch = 0; ch < channels; ++ch) {
            planarOut[ch][frame] = interleaved[(frame * channels) + ch];
        }
    }

    return static_cast<std::size_t>(framesRead);
}

}  // namespace aud::decoder

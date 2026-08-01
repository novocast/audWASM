#include "vorbis_decoder.hpp"

#include <algorithm>

// stb_vorbis.c's implementation is compiled once, as plain C, into aud::third_party (see
// third_party/CMakeLists.txt). Here we only want the declarations, and we need them under C
// linkage to match those C-compiled symbols — a .cpp file including a .c header-only section
// would otherwise get C++-mangled declarations that fail to link.
#define STB_VORBIS_HEADER_ONLY
extern "C" {
#include "stb_vorbis.c"
}

namespace aud::decoder {

namespace {
constexpr std::size_t kMaxHeaderProbeBytes = 1u << 20;
}

struct VorbisDecoder::Impl {
    stb_vorbis* vorbis = nullptr;
};

VorbisDecoder::VorbisDecoder() : m_impl(std::make_unique<Impl>()) {}

VorbisDecoder::~VorbisDecoder() {
    if (m_impl->vorbis != nullptr) {
        stb_vorbis_close(m_impl->vorbis);
    }
}

Result<void> VorbisDecoder::tryInit() {
    auto remaining = m_ring.remaining();
    if (remaining.empty()) {
        return Result<void>{};
    }

    int consumed = 0;
    int error    = 0;
    m_impl->vorbis = stb_vorbis_open_pushdata(
        reinterpret_cast<const unsigned char*>(remaining.data()), static_cast<int>(remaining.size()), &consumed,
        &error, nullptr);

    if (m_impl->vorbis != nullptr) {
        m_ring.consume(static_cast<std::size_t>(consumed));
        m_initialized = true;
        return Result<void>{};
    }

    if (error != VORBIS_need_more_data) {
        return Error{ErrorCode::CorruptData, "decoder.vorbis", "invalid Ogg/Vorbis header"};
    }
    if (m_ring.totalBytes() > kMaxHeaderProbeBytes) {
        return Error{ErrorCode::CorruptData, "decoder.vorbis", "could not parse Ogg/Vorbis header within probe budget"};
    }
    return Result<void>{};
}

Result<void> VorbisDecoder::feed(std::span<const std::byte> bytes) {
    m_ring.feed(bytes.data(), bytes.size());
    if (!m_initialized) {
        return tryInit();
    }
    return Result<void>{};
}

Result<void> VorbisDecoder::signalEndOfInput() {
    m_endOfInput = true;
    if (!m_initialized) {
        AUD_TRY(tryInit());
        if (!m_initialized) {
            return Error{ErrorCode::TruncatedData, "decoder.vorbis", "end of input before a valid header was parsed"};
        }
    }
    return Result<void>{};
}

Result<StreamInfo> VorbisDecoder::info() const {
    if (!m_initialized) {
        return Error{ErrorCode::InvalidArgument, "decoder.vorbis", "not yet initialized"};
    }
    const stb_vorbis_info vi = stb_vorbis_get_info(m_impl->vorbis);
    StreamInfo             si;
    si.sampleRate = vi.sample_rate;
    si.channels   = static_cast<ChannelIndex>(vi.channels);
    si.frameCount = kNoFrame;
    si.codecName  = "vorbis";
    si.bitDepth   = 0;
    si.isLossy    = true;
    si.isEstimate = true;
    return si;
}

Result<std::size_t> VorbisDecoder::read(std::span<std::span<Sample>> planarOut) {
    if (!m_initialized) {
        return std::size_t{0};
    }
    if (planarOut.empty()) {
        return Error{ErrorCode::InvalidArgument, "decoder.vorbis", "no output channels provided"};
    }

    const std::size_t framesRequested = planarOut[0].size();
    std::size_t       framesWritten   = 0;

    // Drain any samples left over from a previous decode call before asking stb_vorbis for more.
    auto drainPending = [&]() {
        while (m_pendingOffset < m_pendingCount && framesWritten < framesRequested) {
            for (std::size_t ch = 0; ch < planarOut.size() && ch < m_pending.size(); ++ch) {
                planarOut[ch][framesWritten] = m_pending[ch][m_pendingOffset];
            }
            ++m_pendingOffset;
            ++framesWritten;
        }
    };
    drainPending();

    while (framesWritten < framesRequested) {
        auto remaining = m_ring.remaining();
        if (remaining.empty()) {
            break;  // need more input
        }

        int     channels = 0;
        float** output   = nullptr;
        int     samples  = 0;
        const int consumed = stb_vorbis_decode_frame_pushdata(
            m_impl->vorbis, reinterpret_cast<const unsigned char*>(remaining.data()),
            static_cast<int>(remaining.size()), &channels, &output, &samples);

        if (consumed == 0) {
            break;  // need more input
        }
        m_ring.consume(static_cast<std::size_t>(consumed));

        if (samples == 0) {
            continue;  // resynchronising; keep going per stb_vorbis's documented contract
        }

        m_pending.assign(static_cast<std::size_t>(channels), {});
        for (int ch = 0; ch < channels; ++ch) {
            m_pending[static_cast<std::size_t>(ch)].assign(output[ch], output[ch] + samples);
        }
        m_pendingOffset = 0;
        m_pendingCount  = static_cast<std::size_t>(samples);

        drainPending();
    }

    return framesWritten;
}

}  // namespace aud::decoder

#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "byte_ring.hpp"
#include "decoder.hpp"

namespace aud::decoder {

// dr_wav-backed decoder. WAV/AIFF headers are small (fmt chunk near the start), so in practice
// init succeeds after the first feed() once at least a few hundred bytes have arrived.
class WavDecoder final : public Decoder {
public:
    WavDecoder();
    ~WavDecoder() override;

    Result<void> feed(std::span<const std::byte> bytes) override;
    Result<void> signalEndOfInput() override;

    [[nodiscard]] Result<StreamInfo> info() const override;
    Result<std::size_t>              read(std::span<std::span<Sample>> planarOut) override;

private:
    Result<void> tryInit();

    // dr_wav computes totalPCMFrameCount/dataChunkSize once, at init time, validated against
    // however many bytes the ring holds *right then* (see onSeek's SEEK_END handling in the .cpp).
    // If init succeeds while only a header-sized prefix has arrived (the common case: JS feeds a
    // WAV in chunks, and the header is far smaller than a single chunk), that snapshot understates
    // the real file — every frame past the header would silently read as "past end of stream".
    // refreshIfGrown() re-runs init whenever the ring has grown since the last (re-)init, so the
    // frame count catches up as more data arrives; m_framesDelivered lets it reseek dr_wav's read
    // cursor past frames already handed to the caller so a refresh never re-delivers them.
    Result<void> refreshIfGrown();

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    ByteRing      m_ring;
    bool          m_initialized        = false;
    bool          m_endOfInput         = false;
    std::size_t   m_initAttempts       = 0;
    std::size_t   m_lastRingBytesAtInit = 0;
    std::uint64_t m_framesDelivered    = 0;
};

}  // namespace aud::decoder

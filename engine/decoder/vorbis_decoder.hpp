#pragma once

#include <memory>
#include <span>
#include <vector>

#include "byte_ring.hpp"
#include "decoder.hpp"

namespace aud::decoder {

// stb_vorbis pushdata-API-backed decoder. Unlike the dr_libs wrappers this needs no read/seek
// callback bridge: stb_vorbis's pushdata API already takes a raw pointer + length and reports back
// how many bytes it consumed, which maps directly onto ByteRing's remaining()/consume().
class VorbisDecoder final : public Decoder {
public:
    VorbisDecoder();
    ~VorbisDecoder() override;

    Result<void> feed(std::span<const std::byte> bytes) override;
    Result<void> signalEndOfInput() override;

    [[nodiscard]] Result<StreamInfo> info() const override;
    Result<std::size_t>              read(std::span<std::span<Sample>> planarOut) override;

private:
    Result<void> tryInit();

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    ByteRing    m_ring;
    bool        m_initialized = false;
    bool        m_endOfInput  = false;

    // Leftover decoded samples from a stb_vorbis_get_frame_float-ish call that produced more
    // samples than the caller's planarOut could hold in one go.
    std::vector<std::vector<Sample>> m_pending;
    std::size_t                      m_pendingOffset = 0;
    std::size_t                      m_pendingCount  = 0;
};

}  // namespace aud::decoder

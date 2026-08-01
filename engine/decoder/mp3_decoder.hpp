#pragma once

#include <memory>
#include <span>

#include "byte_ring.hpp"
#include "decoder.hpp"

namespace aud::decoder {

// dr_mp3 (minimp3 core) backed decoder. MP3 has no reliable frame-count header of its own; we take
// it from a Xing/LAME/VBRI header when present (dr_mp3 does not parse these for us, so frameCount
// is reported as an estimate from bitrate*duration until M02's Xing/LAME task lands) — see M02
// task list. Encoder delay/padding likewise require Xing/LAME parsing, tracked as a follow-up.
class Mp3Decoder final : public Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder() override;

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
};

}  // namespace aud::decoder

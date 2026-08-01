#pragma once

#include <memory>
#include <span>

#include "byte_ring.hpp"
#include "decoder.hpp"

namespace aud::decoder {

// dr_flac-backed decoder. Handles native and Ogg-FLAC (dr_flac auto-detects the container).
class FlacDecoder final : public Decoder {
public:
    FlacDecoder();
    ~FlacDecoder() override;

    Result<void> feed(std::span<const std::byte> bytes) override;
    Result<void> signalEndOfInput() override;

    [[nodiscard]] Result<StreamInfo> info() const override;
    Result<std::size_t>              read(std::span<std::span<Sample>> planarOut) override;

private:
    Result<void> tryInit();

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    ByteRing    m_ring;
    bool        m_initialized  = false;
    bool        m_endOfInput   = false;
};

}  // namespace aud::decoder

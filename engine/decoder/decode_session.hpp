#pragma once

// Orchestrates sniff -> createDecoder -> AudioBuffer, firing progress callbacks as chunks land.
// See M02 "Progressive decode". This is the seam M20's per-chunk analyser dispatch will hang off
// of later; for now it just fills an AudioBuffer.

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "../util/audio_buffer.hpp"
#include "../util/result.hpp"
#include "decoder.hpp"

namespace aud::decoder {

struct DecodeProgress {
    FrameIndex framesDecoded  = 0;
    FrameIndex estimatedTotal = kNoFrame;  // kNoFrame if genuinely unknown
    double     seconds        = 0.0;
    bool       isEstimate     = true;
};

class DecodeSession {
public:
    using ProgressCallback = std::function<void(const DecodeProgress&)>;

    DecodeSession(const DecodeSession&)            = delete;
    DecodeSession& operator=(const DecodeSession&) = delete;
    DecodeSession(DecodeSession&&) noexcept        = default;
    DecodeSession& operator=(DecodeSession&&) noexcept = default;

    // `probeBytes` should be the first slice fed (>= a few KB; 64KB recommended per M02's sniffing
    // ladder). It is sniffed here but must also be passed to the first feed() call — this only
    // identifies the format, it does not consume the bytes.
    static Result<DecodeSession> create(std::span<const std::byte> probeBytes, Allocator& allocator = defaultAllocator());

    // Feeds the next slice of file bytes (e.g. a 256KB JS-side read, per M02), decodes whatever
    // frames are now available, appends them to the internal AudioBuffer, and fires the progress
    // callback once if any frames were decoded.
    Result<void> feed(std::span<const std::byte> bytes);

    // Signals no more bytes are coming and drains any remaining buffered/queued frames.
    Result<void> finish();

    void setProgressCallback(ProgressCallback callback) { m_progress = std::move(callback); }

    [[nodiscard]] Result<StreamInfo> streamInfo() const;
    [[nodiscard]] const AudioBuffer* buffer() const noexcept { return m_buffer ? &*m_buffer : nullptr; }

    // Moves the decoded buffer out. Only call once decoding is complete (after finish()).
    [[nodiscard]] AudioBuffer takeBuffer() { return std::move(*m_buffer); }

    [[nodiscard]] std::vector<DecodeDiagnostic> takeDiagnostics() { return m_decoder->takeDiagnostics(); }

private:
    explicit DecodeSession(std::unique_ptr<Decoder> decoder, Allocator& allocator)
        : m_decoder(std::move(decoder)), m_allocator(&allocator) {}

    Result<void> drainAvailableFrames();
    Result<void> ensureBufferCreated();

    std::unique_ptr<Decoder>    m_decoder;
    Allocator*                  m_allocator;
    std::optional<AudioBuffer>  m_buffer;
    ProgressCallback            m_progress;

    static constexpr std::size_t kReadChunkFrames = 4096;
};

}  // namespace aud::decoder

#pragma once

// Decoder interface. See M02 for the full design rationale (push model, streaming, encoder
// delay/padding reporting).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../util/audio_types.hpp"
#include "../util/result.hpp"

namespace aud::decoder {

struct StreamInfo {
    SampleRate    sampleRate     = 0;
    ChannelIndex  channels       = 0;
    FrameIndex    frameCount     = -1;  // -1 == unknown until fully decoded
    std::string   codecName;            // "wav", "flac", "mp3", "vorbis", "external"
    std::uint32_t bitDepth       = 0;   // source bit depth, 0 if lossy
    std::uint32_t nominalBitrate = 0;
    bool          isLossy        = false;
    bool          isEstimate     = false;  // true if frameCount is a size/bitrate estimate

    // Encoder delay/padding, populated where the container tells us (LAME/Xing, iTunSMPB, FLAC).
    std::uint32_t encoderDelayFrames   = 0;
    std::uint32_t encoderPaddingFrames = 0;
};

enum class DiagnosticSeverity : std::uint8_t { Info, Warning, Error };

struct DecodeDiagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    ErrorCode          code     = ErrorCode::Ok;
    std::int64_t       byteOffset  = -1;
    FrameIndex         frameIndex  = kNoFrame;
    std::string        message;
};

// Push-model streaming decoder. The host feeds bytes as they arrive; the decoder emits planar
// float frames on demand via read(). See byte_ring.hpp for how the dr_libs-backed implementations
// bridge this to those libraries' pull-style read callbacks.
class Decoder {
public:
    virtual ~Decoder() = default;

    virtual Result<void> feed(std::span<const std::byte> bytes) = 0;
    virtual Result<void> signalEndOfInput()                     = 0;

    [[nodiscard]] virtual Result<StreamInfo> info() const = 0;

    // Emits into caller-provided planar spans (one per channel); returns frames written. 0 means
    // "need more input" (call feed() again) unless signalEndOfInput() was already called and this
    // decoder is genuinely exhausted.
    virtual Result<std::size_t> read(std::span<std::span<Sample>> planarOut) = 0;

    [[nodiscard]] virtual std::vector<DecodeDiagnostic> takeDiagnostics() { return {}; }

    [[nodiscard]] virtual bool  canSeek() const noexcept { return false; }
    virtual Result<void>        seekToFrame(FrameIndex) { return Error{ErrorCode::NotImplemented, "decoder"}; }
};

// Sniffs `probeBytes` (at least the first 64KB, per M02's detection ladder) and constructs the
// matching decoder. Does not consume the bytes — the caller must still feed() them to the returned
// decoder.
Result<std::unique_ptr<Decoder>> createDecoder(std::span<const std::byte> probeBytes);

}  // namespace aud::decoder

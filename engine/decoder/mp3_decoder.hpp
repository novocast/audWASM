#pragma once

#include <cstdint>
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

    // drmp3_init validates the Xing/VBRI header's declared audio byte range against however many
    // bytes the ring holds *right then* (same SEEK_END-at-partial-ring hazard as wav_decoder.cpp's
    // onSeek). Progressive feed (main.ts/decodeWorker.ts feed a probe of a few MB, then the rest in
    // small slices) means init commonly succeeds while only a prefix of the file has arrived — real
    // MP3s routinely carry a multi-MB ID3v2 tag (embedded cover art) ahead of the audio frames, so
    // that prefix can be well short of the full file. Once dr_mp3 has latched onto that undersized
    // view, drmp3_read_pcm_frames_f32 reports EOF once it reaches it, even though the rest of the
    // file's bytes are sitting in the ring, already fed. refreshIfGrown() re-runs init whenever the
    // ring has grown since the last (re-)init, so dr_mp3's understanding of the stream catches up as
    // more data arrives; m_framesDelivered lets it reseek dr_mp3's read cursor past frames already
    // handed to the caller so a refresh never re-delivers them.
    Result<void> refreshIfGrown();

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    ByteRing      m_ring;
    bool          m_initialized         = false;
    bool          m_endOfInput          = false;
    std::size_t   m_lastRingBytesAtInit = 0;
    std::uint64_t m_framesDelivered     = 0;
};

}  // namespace aud::decoder

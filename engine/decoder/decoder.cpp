#include "decoder.hpp"

#include "flac_decoder.hpp"
#include "format_sniffer.hpp"
#include "mp3_decoder.hpp"
#include "vorbis_decoder.hpp"
#include "wav_decoder.hpp"

namespace aud::decoder {

Result<std::unique_ptr<Decoder>> createDecoder(std::span<const std::byte> probeBytes) {
    AUD_TRY_ASSIGN(sniffed, sniff(probeBytes));

    switch (sniffed.format) {
        case ContainerFormat::Wav:
            return std::unique_ptr<Decoder>(std::make_unique<WavDecoder>());
        case ContainerFormat::Flac:
            return std::unique_ptr<Decoder>(std::make_unique<FlacDecoder>());
        case ContainerFormat::Mp3:
            return std::unique_ptr<Decoder>(std::make_unique<Mp3Decoder>());
        case ContainerFormat::OggVorbis:
            return std::unique_ptr<Decoder>(std::make_unique<VorbisDecoder>());
        case ContainerFormat::Aiff:
            return Error{ErrorCode::UnsupportedFormat, "decoder",
                         "AIFF is a post-v1 stretch target (see M02); not yet implemented"};
        case ContainerFormat::Mp4Aac:
            return Error{ErrorCode::UnsupportedFormat, "decoder",
                         "AAC/M4A is decoded via the browser fallback (ExternalPcmSource), not the "
                         "native decoder path — see M02 'The AAC problem'"};
        case ContainerFormat::Mp4Other:
            return Error{ErrorCode::UnsupportedFormat, "decoder", "MP4 container does not look like audio-only M4A/AAC"};
        case ContainerFormat::Unknown:
            break;
    }
    return Error{ErrorCode::UnsupportedFormat, "decoder", "unrecognised or unhandled container format"};
}

}  // namespace aud::decoder

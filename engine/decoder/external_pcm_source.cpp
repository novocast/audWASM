#include "external_pcm_source.hpp"

namespace aud::decoder {

Result<void> ingestExternalPcm(AudioBuffer& buffer, std::span<const std::span<const Sample>> planarChannels,
                                std::size_t frameCount) {
    if (planarChannels.size() != buffer.channelCount()) {
        return Error{ErrorCode::InvalidArgument, "decoder.external_pcm", "channel count mismatch with target buffer"};
    }
    return buffer.append(planarChannels, frameCount);
}

}  // namespace aud::decoder

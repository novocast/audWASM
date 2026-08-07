#include "channel_weights.hpp"

#include "../../util/logging.hpp"

namespace aud::loudness {

namespace {
constexpr std::string_view kLogDomain = "analysis.loudness.channels";
}  // namespace

double weightForRole(ChannelRole role) noexcept {
    switch (role) {
        case ChannelRole::FrontLeft:
        case ChannelRole::FrontRight:
        case ChannelRole::FrontCenter:
        case ChannelRole::Other:
            return 1.0;
        case ChannelRole::BackLeft:
        case ChannelRole::BackRight:
            return 1.41;  // +1.5 dB, BS.1770 surround weighting
        case ChannelRole::Lfe:
            return 0.0;  // excluded entirely
    }
    return 1.0;
}

ChannelWeightResolution resolveChannelRolesByCount(ChannelIndex channelCount) {
    ChannelWeightResolution result;
    result.weights.assign(channelCount, 1.0);
    result.usedFallback = true;

    switch (channelCount) {
        case 0:
            break;
        case 1:
            // Mono: BS.1770 treats the single channel as centre, weight 1.0 (already the default).
            break;
        case 2:
            // Stereo L/R, both weight 1.0 (already the default).
            break;
        case 5:
            // 5.0 (no LFE): L R C Ls Rs — e.g. EBU Tech 3341 test signal #6's 5-channel variant.
            result.weights[0] = weightForRole(ChannelRole::FrontLeft);
            result.weights[1] = weightForRole(ChannelRole::FrontRight);
            result.weights[2] = weightForRole(ChannelRole::FrontCenter);
            result.weights[3] = weightForRole(ChannelRole::BackLeft);
            result.weights[4] = weightForRole(ChannelRole::BackRight);
            break;
        case 6:
            // Standard WAV 5.1 order: L R C LFE Ls Rs.
            result.weights[0] = weightForRole(ChannelRole::FrontLeft);
            result.weights[1] = weightForRole(ChannelRole::FrontRight);
            result.weights[2] = weightForRole(ChannelRole::FrontCenter);
            result.weights[3] = weightForRole(ChannelRole::Lfe);
            result.weights[4] = weightForRole(ChannelRole::BackLeft);
            result.weights[5] = weightForRole(ChannelRole::BackRight);
            break;
        default:
            // No documented convention for this channel count without a container-supplied
            // layout — assume every channel is weighted 1.0 and say so loudly (M08 risk table:
            // "never silently guess").
            AUD_LOG_WARN(kLogDomain,
                         "no documented channel-layout assumption for this channel count; "
                         "weighting every channel 1.0 (LFE exclusion not applied)");
            break;
    }
    return result;
}

ChannelWeightResolution resolveChannelRolesFromWavMask(ChannelIndex channelCount, std::uint32_t channelMask) {
    ChannelWeightResolution result;
    result.weights.assign(channelCount, 1.0);
    result.usedFallback = false;

    // WAVE_FORMAT_EXTENSIBLE channels are ordered by increasing speaker-bit value among the bits
    // actually set in the mask (Microsoft's documented convention), not by a fixed slot layout.
    static constexpr WavSpeakerBit kBitsInOrder[] = {
        WavSpeakerBit::FrontLeft, WavSpeakerBit::FrontRight,  WavSpeakerBit::FrontCenter,
        WavSpeakerBit::LowFrequency, WavSpeakerBit::BackLeft, WavSpeakerBit::BackRight,
        WavSpeakerBit::SideLeft,     WavSpeakerBit::SideRight,
    };

    std::size_t channelIndex = 0;
    for (WavSpeakerBit bit : kBitsInOrder) {
        if (channelIndex >= channelCount) break;
        if ((channelMask & static_cast<std::uint32_t>(bit)) == 0) continue;

        ChannelRole role = ChannelRole::Other;
        switch (bit) {
            case WavSpeakerBit::FrontLeft:    role = ChannelRole::FrontLeft; break;
            case WavSpeakerBit::FrontRight:   role = ChannelRole::FrontRight; break;
            case WavSpeakerBit::FrontCenter:  role = ChannelRole::FrontCenter; break;
            case WavSpeakerBit::LowFrequency: role = ChannelRole::Lfe; break;
            case WavSpeakerBit::BackLeft:
            case WavSpeakerBit::SideLeft:     role = ChannelRole::BackLeft; break;
            case WavSpeakerBit::BackRight:
            case WavSpeakerBit::SideRight:    role = ChannelRole::BackRight; break;
        }
        result.weights[channelIndex] = weightForRole(role);
        ++channelIndex;
    }

    if (channelIndex < channelCount) {
        AUD_LOG_WARN(kLogDomain,
                     "channel mask described fewer positions than the stream has channels; "
                     "remaining channels weighted 1.0");
        result.usedFallback = true;
    }
    return result;
}

}  // namespace aud::loudness

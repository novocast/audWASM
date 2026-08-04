#include "waveform_store.hpp"

#include <algorithm>
#include <cmath>

#include "reduce.hpp"

namespace aud::waveform {

namespace {
constexpr std::string_view kDomain = "waveform.store";

// Combines the level-0 bins [loBin, hiBin) into a single bin. RMS is combined by weight (frame
// count) rather than averaged naively, since sqrt(mean(x^2)) values don't average linearly:
// combining N bins' RMS correctly requires recovering each bin's sum-of-squares
// (rms^2 * frameCount) before summing. `totalFrames` is the channel's overall frame count, needed
// to work out how many frames the final (possibly short) bin actually covers.
WaveformBin combineRange(std::span<const WaveformBin> srcBins, std::size_t loBin, std::size_t hiBin,
                          FrameIndex totalFrames) {
    WaveformBin out;
    if (loBin >= hiBin || hiBin > srcBins.size()) {
        return out;
    }

    Sample      lo                 = srcBins[loBin].min;
    Sample      hi                 = srcBins[loBin].max;
    double      sumSqWeighted      = 0.0;
    std::uint64_t totalCoveredFrames = 0;

    for (std::size_t i = loBin; i < hiBin; ++i) {
        const WaveformBin& b = srcBins[i];
        lo = std::min(lo, b.min);
        hi = std::max(hi, b.max);

        const auto  frameStart   = static_cast<std::uint64_t>(i) * kBaseBinFrames;
        const auto  frameEnd     = std::min<std::uint64_t>(frameStart + kBaseBinFrames,
                                                            static_cast<std::uint64_t>(std::max<FrameIndex>(totalFrames, 0)));
        const auto  framesInBin  = frameEnd > frameStart ? frameEnd - frameStart : 0;

        sumSqWeighted += static_cast<double>(b.rms) * static_cast<double>(b.rms) * static_cast<double>(framesInBin);
        totalCoveredFrames += framesInBin;
    }

    out.min     = lo;
    out.max     = hi;
    out.rms     = totalCoveredFrames > 0
                      ? static_cast<Sample>(std::sqrt(sumSqWeighted / static_cast<double>(totalCoveredFrames)))
                      : 0.0f;
    out.absPeak = std::max(-lo, hi);
    return out;
}

}  // namespace

void WaveformStore::reset(ChannelIndex channelCount) {
    m_perChannelBins.assign(channelCount, {});
    m_monoSum.reset();
    m_mid.reset();
    m_side.reset();
    m_complete = false;
}

void WaveformStore::appendChunk(ChannelIndex ch, std::span<const Sample> chunkSamples) {
    if (ch >= m_perChannelBins.size()) {
        return;
    }
    reduceToBins(chunkSamples, kBaseBinFrames, m_perChannelBins[ch]);
}

std::span<const WaveformBin> WaveformStore::bins(ChannelIndex ch) const noexcept {
    if (ch >= m_perChannelBins.size()) {
        return {};
    }
    return m_perChannelBins[ch];
}

Result<std::span<const WaveformBin>> WaveformStore::monoSumBins(const AudioBuffer& buffer) {
    if (m_monoSum.has_value()) {
        return std::span<const WaveformBin>(*m_monoSum);
    }

    const ChannelIndex channels = buffer.channelCount();
    if (channels == 0) {
        return Error{ErrorCode::InvalidArgument, kDomain, "mono-sum requires at least one channel"};
    }

    std::vector<WaveformBin> out;
    std::vector<Sample>      scratch;
    const std::size_t        chunkCount = buffer.chunkCount();
    const Sample              scale     = 1.0f / static_cast<Sample>(channels);

    for (std::size_t c = 0; c < chunkCount; ++c) {
        std::span<const Sample> first = buffer.chunk(0, c);
        scratch.assign(first.size(), 0.0f);
        for (ChannelIndex ch = 0; ch < channels; ++ch) {
            std::span<const Sample> chSpan = buffer.chunk(ch, c);
            for (std::size_t i = 0; i < chSpan.size(); ++i) {
                scratch[i] += chSpan[i];
            }
        }
        for (Sample& s : scratch) {
            s *= scale;
        }
        reduceToBins(scratch, kBaseBinFrames, out);
    }

    m_monoSum = std::move(out);
    return std::span<const WaveformBin>(*m_monoSum);
}

Result<void> WaveformStore::ensureMidSide(const AudioBuffer& buffer) {
    if (m_mid.has_value() && m_side.has_value()) {
        return {};
    }
    if (buffer.channelCount() != 2) {
        return Error{ErrorCode::InvalidArgument, kDomain, "mid/side requires exactly 2 (stereo) channels"};
    }

    std::vector<WaveformBin> mid;
    std::vector<WaveformBin> side;
    std::vector<Sample>      midScratch;
    std::vector<Sample>      sideScratch;

    const std::size_t chunkCount = buffer.chunkCount();
    for (std::size_t c = 0; c < chunkCount; ++c) {
        std::span<const Sample> l = buffer.chunk(0, c);
        std::span<const Sample> r = buffer.chunk(1, c);
        const std::size_t       n = l.size();
        midScratch.resize(n);
        sideScratch.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            midScratch[i]  = (l[i] + r[i]) * 0.5f;
            sideScratch[i] = (l[i] - r[i]) * 0.5f;
        }
        reduceToBins(midScratch, kBaseBinFrames, mid);
        reduceToBins(sideScratch, kBaseBinFrames, side);
    }

    m_mid  = std::move(mid);
    m_side = std::move(side);
    return {};
}

Result<std::span<const WaveformBin>> WaveformStore::midBins(const AudioBuffer& buffer) {
    AUD_TRY(ensureMidSide(buffer));
    return std::span<const WaveformBin>(*m_mid);
}

Result<std::span<const WaveformBin>> WaveformStore::sideBins(const AudioBuffer& buffer) {
    AUD_TRY(ensureMidSide(buffer));
    return std::span<const WaveformBin>(*m_side);
}

Result<WaveformView> WaveformStore::query(const WaveformRequest& request, const AudioBuffer* buffer) {
    if (request.binCount == 0 || request.range.empty()) {
        return Error{ErrorCode::InvalidArgument, kDomain, "binCount and range must be non-empty"};
    }
    if (buffer == nullptr) {
        return Error{ErrorCode::InvalidArgument, kDomain, "query() requires the source AudioBuffer"};
    }

    std::vector<std::span<const WaveformBin>> sourceChannels;
    switch (request.channels) {
        case ChannelSelector::PerChannel:
            for (ChannelIndex ch = 0; ch < channelCount(); ++ch) {
                sourceChannels.push_back(bins(ch));
            }
            break;
        case ChannelSelector::MonoSum: {
            AUD_TRY_ASSIGN(b, monoSumBins(*buffer));
            sourceChannels.push_back(b);
            break;
        }
        case ChannelSelector::MidSide: {
            AUD_TRY_ASSIGN(m, midBins(*buffer));
            AUD_TRY_ASSIGN(s, sideBins(*buffer));
            sourceChannels.push_back(m);
            sourceChannels.push_back(s);
            break;
        }
    }
    if (sourceChannels.empty()) {
        return Error{ErrorCode::InvalidArgument, kDomain, "no channels available"};
    }

    const auto channels = static_cast<std::uint32_t>(sourceChannels.size());
    m_queryScratch.assign(static_cast<std::size_t>(channels) * request.binCount, WaveformBin{});

    const FrameIndex totalFrames = buffer->frameCount();
    const FrameIndex rangeBegin  = std::clamp<FrameIndex>(request.range.begin, 0, totalFrames);
    const FrameIndex rangeEnd    = std::clamp<FrameIndex>(request.range.end, rangeBegin, totalFrames);
    const auto        rangeFrames = static_cast<std::uint64_t>(rangeEnd - rangeBegin);
    const auto        framesPerBin =
        rangeFrames == 0 ? 0u : static_cast<std::uint32_t>(std::max<std::uint64_t>(1, rangeFrames / request.binCount));

    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        std::span<const WaveformBin> srcBins = sourceChannels[ch];
        for (std::uint32_t i = 0; i < request.binCount; ++i) {
            const std::uint64_t frameStart = static_cast<std::uint64_t>(rangeBegin) + (rangeFrames * i) / request.binCount;
            const std::uint64_t frameEndExcl =
                static_cast<std::uint64_t>(rangeBegin) + (rangeFrames * (i + 1)) / request.binCount;

            std::size_t loBin = static_cast<std::size_t>(frameStart / kBaseBinFrames);
            std::size_t hiBin = frameEndExcl == frameStart
                                     ? loBin + 1
                                     : static_cast<std::size_t>((frameEndExcl - 1) / kBaseBinFrames) + 1;
            loBin = std::min(loBin, srcBins.size());
            hiBin = std::min(hiBin, srcBins.size());

            m_queryScratch[static_cast<std::size_t>(ch) * request.binCount + i] =
                combineRange(srcBins, loBin, hiBin, totalFrames);
        }
    }

    WaveformView view;
    view.data         = m_queryScratch.data();
    view.channels     = channels;
    view.binCount     = request.binCount;
    view.framesPerBin = framesPerBin;
    view.isComplete   = m_complete;
    return view;
}

}  // namespace aud::waveform

#include "waveform_store.hpp"

#include <algorithm>
#include <cmath>

#include "reduce.hpp"

namespace aud::waveform {

namespace {
constexpr std::string_view kDomain = "waveform.store";

// Raw-PCM fallback for one output bin when the requested resolution is finer than level 0 (M05
// "Below level 0"). `variantIndex` selects which physical channel to read for PerChannel, is
// ignored for MonoSum (every channel is averaged), and picks mid (0) vs side (1) for MidSide.
Result<WaveformBin> rawPcmBin(const AudioBuffer& buffer, ChannelSelector selector, ChannelIndex variantIndex,
                               FrameIndex lo, FrameIndex hi, std::vector<Sample>& scratch) {
    if (hi <= lo) {
        return WaveformBin{};
    }
    const auto count = static_cast<std::size_t>(hi - lo);

    switch (selector) {
        case ChannelSelector::PerChannel: {
            scratch.resize(count);
            AUD_TRY(buffer.read(variantIndex, FrameRange{lo, hi}, scratch));
            return reduceOneBin(scratch);
        }
        case ChannelSelector::MonoSum: {
            const ChannelIndex  channels = buffer.channelCount();
            std::vector<Sample> chScratch(count);
            scratch.assign(count, 0.0f);
            for (ChannelIndex ch = 0; ch < channels; ++ch) {
                AUD_TRY(buffer.read(ch, FrameRange{lo, hi}, chScratch));
                for (std::size_t i = 0; i < count; ++i) {
                    scratch[i] += chScratch[i];
                }
            }
            const Sample scale = 1.0f / static_cast<Sample>(std::max<ChannelIndex>(channels, 1));
            for (Sample& s : scratch) {
                s *= scale;
            }
            return reduceOneBin(scratch);
        }
        case ChannelSelector::MidSide: {
            std::vector<Sample> l(count);
            std::vector<Sample> r(count);
            AUD_TRY(buffer.read(0, FrameRange{lo, hi}, l));
            AUD_TRY(buffer.read(1, FrameRange{lo, hi}, r));
            scratch.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                scratch[i] = variantIndex == 0 ? (l[i] + r[i]) * 0.5f : (l[i] - r[i]) * 0.5f;
            }
            return reduceOneBin(scratch);
        }
    }
    return WaveformBin{};
}

}  // namespace

void WaveformStore::reset(ChannelIndex channelCount) {
    m_perChannelPyramid.reset(channelCount);
    m_monoSum.reset();
    m_mid.reset();
    m_side.reset();
    m_complete = false;
}

void WaveformStore::appendChunk(ChannelIndex ch, std::span<const Sample> chunkSamples) {
    if (ch >= channelCount() || chunkSamples.empty()) {
        return;
    }
    m_level0Scratch.clear();
    reduceToBins(chunkSamples, kBaseBinFrames, m_level0Scratch);
    m_perChannelPyramid.appendLevel0Bins(ch, m_level0Scratch, chunkSamples.size());
}

void WaveformStore::markComplete() {
    m_complete = true;
    m_perChannelPyramid.finalize();
}

std::span<const WaveformBin> WaveformStore::bins(ChannelIndex ch) const noexcept {
    return m_perChannelPyramid.level(ch, 0);
}

Result<std::span<const WaveformBin>> WaveformStore::monoSumBins(const AudioBuffer& buffer) {
    if (m_monoSum.has_value()) {
        return m_monoSum->level(0, 0);
    }

    const ChannelIndex channels = buffer.channelCount();
    if (channels == 0) {
        return Error{ErrorCode::InvalidArgument, kDomain, "mono-sum requires at least one channel"};
    }

    WaveformPyramid pyramid;
    pyramid.reset(1);

    std::vector<WaveformBin> reduced;
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
        reduced.clear();
        reduceToBins(scratch, kBaseBinFrames, reduced);
        pyramid.appendLevel0Bins(0, reduced, scratch.size());
    }
    pyramid.finalize();

    m_monoSum = std::move(pyramid);
    return m_monoSum->level(0, 0);
}

Result<void> WaveformStore::ensureMidSide(const AudioBuffer& buffer) {
    if (m_mid.has_value() && m_side.has_value()) {
        return {};
    }
    if (buffer.channelCount() != 2) {
        return Error{ErrorCode::InvalidArgument, kDomain, "mid/side requires exactly 2 (stereo) channels"};
    }

    WaveformPyramid mid;
    WaveformPyramid side;
    mid.reset(1);
    side.reset(1);

    std::vector<WaveformBin> midReduced;
    std::vector<WaveformBin> sideReduced;
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
        midReduced.clear();
        sideReduced.clear();
        reduceToBins(midScratch, kBaseBinFrames, midReduced);
        reduceToBins(sideScratch, kBaseBinFrames, sideReduced);
        mid.appendLevel0Bins(0, midReduced, n);
        side.appendLevel0Bins(0, sideReduced, n);
    }
    mid.finalize();
    side.finalize();

    m_mid  = std::move(mid);
    m_side = std::move(side);
    return {};
}

Result<std::span<const WaveformBin>> WaveformStore::midBins(const AudioBuffer& buffer) {
    AUD_TRY(ensureMidSide(buffer));
    return m_mid->level(0, 0);
}

Result<std::span<const WaveformBin>> WaveformStore::sideBins(const AudioBuffer& buffer) {
    AUD_TRY(ensureMidSide(buffer));
    return m_side->level(0, 0);
}

Result<WaveformView> WaveformStore::query(const WaveformRequest& request, const AudioBuffer* buffer) {
    if (request.binCount == 0 || request.range.empty()) {
        return Error{ErrorCode::InvalidArgument, kDomain, "binCount and range must be non-empty"};
    }
    if (buffer == nullptr) {
        return Error{ErrorCode::InvalidArgument, kDomain, "query() requires the source AudioBuffer"};
    }

    std::vector<const WaveformPyramid*> pyramids;
    std::vector<ChannelIndex>           pyramidChannel;  // channel index *within* each pyramid above
    switch (request.channels) {
        case ChannelSelector::PerChannel:
            for (ChannelIndex ch = 0; ch < channelCount(); ++ch) {
                pyramids.push_back(&m_perChannelPyramid);
                pyramidChannel.push_back(ch);
            }
            break;
        case ChannelSelector::MonoSum: {
            AUD_TRY_ASSIGN(unused, monoSumBins(*buffer));
            (void)unused;
            pyramids.push_back(&*m_monoSum);
            pyramidChannel.push_back(0);
            break;
        }
        case ChannelSelector::MidSide: {
            AUD_TRY(ensureMidSide(*buffer));
            pyramids.push_back(&*m_mid);
            pyramidChannel.push_back(0);
            pyramids.push_back(&*m_side);
            pyramidChannel.push_back(0);
            break;
        }
    }
    if (pyramids.empty()) {
        return Error{ErrorCode::InvalidArgument, kDomain, "no channels available"};
    }

    const auto channels = static_cast<std::uint32_t>(pyramids.size());
    m_queryScratch.assign(static_cast<std::size_t>(channels) * request.binCount, WaveformBin{});

    const FrameIndex totalFrames = buffer->frameCount();
    const FrameIndex rangeBegin  = std::clamp<FrameIndex>(request.range.begin, 0, totalFrames);
    const FrameIndex rangeEnd    = std::clamp<FrameIndex>(request.range.end, rangeBegin, totalFrames);
    const auto        rangeFrames = static_cast<std::uint64_t>(rangeEnd - rangeBegin);
    const auto        framesPerBinReported =
        rangeFrames == 0 ? 0u : static_cast<std::uint32_t>(std::max<std::uint64_t>(1, rangeFrames / request.binCount));
    const std::uint64_t framesPerRequestedBin = std::max<std::uint64_t>(1, rangeFrames / request.binCount);

    bool                 anyRawPcm = false;
    std::vector<Sample>  rawScratch;

    for (std::uint32_t outCh = 0; outCh < channels; ++outCh) {
        const WaveformPyramid& pyramid = *pyramids[outCh];
        const ChannelIndex      srcCh   = pyramidChannel[outCh];
        const std::uint32_t     level   = selectLevel(framesPerRequestedBin, pyramid.maxLevel(srcCh));

        std::span<const WaveformBin> srcBins;
        std::uint32_t                 framesPerSrcBin = 0;
        std::uint32_t                 trailing        = 0;
        if (level != kRawPcmLevel) {
            srcBins         = pyramid.level(srcCh, level);
            framesPerSrcBin = WaveformPyramid::framesPerBin(level);
            trailing        = pyramid.trailingFrameCount(srcCh, level);
        }

        for (std::uint32_t i = 0; i < request.binCount; ++i) {
            const std::uint64_t frameStart   = static_cast<std::uint64_t>(rangeBegin) + (rangeFrames * i) / request.binCount;
            const std::uint64_t frameEndExcl = static_cast<std::uint64_t>(rangeBegin) + (rangeFrames * (i + 1)) / request.binCount;

            WaveformBin outBin;
            if (level == kRawPcmLevel) {
                anyRawPcm = true;
                const auto lo     = static_cast<FrameIndex>(frameStart);
                const auto hi     = static_cast<FrameIndex>(std::max(frameEndExcl, frameStart + 1));
                const auto hiClamped = std::min<FrameIndex>(hi, totalFrames);
                auto        result   = rawPcmBin(*buffer, request.channels, outCh, lo, hiClamped, rawScratch);
                if (result.has_value()) {
                    outBin = result.value();
                }
            } else {
                outBin = aggregateRange(srcBins, framesPerSrcBin, trailing, frameStart, frameEndExcl);
            }
            m_queryScratch[static_cast<std::size_t>(outCh) * request.binCount + i] = outBin;
        }
    }

    WaveformView view;
    view.data         = m_queryScratch.data();
    view.channels     = channels;
    view.binCount     = request.binCount;
    view.framesPerBin = framesPerBinReported;
    view.isComplete   = m_complete;
    view.isRawPcm     = anyRawPcm;
    return view;
}

}  // namespace aud::waveform

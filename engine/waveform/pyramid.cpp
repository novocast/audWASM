#include "pyramid.hpp"

#include <algorithm>
#include <cmath>

namespace aud::waveform {

WaveformBin fold(const WaveformBin& a, std::uint32_t framesA, const WaveformBin& b, std::uint32_t framesB) noexcept {
    WaveformBin out;
    out.min     = std::min(a.min, b.min);
    out.max     = std::max(a.max, b.max);
    out.absPeak = std::max(a.absPeak, b.absPeak);

    const double        sumSq      = static_cast<double>(framesA) * static_cast<double>(a.rms) * static_cast<double>(a.rms) +
                                      static_cast<double>(framesB) * static_cast<double>(b.rms) * static_cast<double>(b.rms);
    const std::uint64_t totalFrames = static_cast<std::uint64_t>(framesA) + framesB;
    out.rms = totalFrames > 0 ? static_cast<Sample>(std::sqrt(sumSq / static_cast<double>(totalFrames))) : 0.0f;
    return out;
}

std::uint32_t selectLevel(std::uint64_t framesPerRequestedBin, std::uint32_t maxLevel) noexcept {
    if (framesPerRequestedBin < kBaseBinFrames) {
        return kRawPcmLevel;
    }
    const double ratio = static_cast<double>(framesPerRequestedBin) / static_cast<double>(kBaseBinFrames);
    const auto   level = static_cast<std::uint32_t>(std::floor(std::log2(ratio)));
    return std::min(level, maxLevel);
}

WaveformBin aggregateRange(std::span<const WaveformBin> srcBins, std::uint32_t framesPerSrcBin,
                            std::uint32_t trailingFrameCount, std::uint64_t frameStart, std::uint64_t frameEnd) noexcept {
    WaveformBin out;
    if (srcBins.empty() || frameEnd <= frameStart || framesPerSrcBin == 0) {
        return out;
    }

    std::size_t loBin    = static_cast<std::size_t>(frameStart / framesPerSrcBin);
    std::size_t hiBinExcl = static_cast<std::size_t>((frameEnd - 1) / framesPerSrcBin) + 1;
    loBin                 = std::min(loBin, srcBins.size());
    hiBinExcl             = std::min(hiBinExcl, srcBins.size());
    if (loBin >= hiBinExcl) {
        return out;
    }

    Sample        lo            = srcBins[loBin].min;
    Sample        hi            = srcBins[loBin].max;
    double        sumSqWeighted = 0.0;
    std::uint64_t coveredFrames = 0;

    for (std::size_t i = loBin; i < hiBinExcl; ++i) {
        const WaveformBin& b = srcBins[i];
        lo = std::min(lo, b.min);
        hi = std::max(hi, b.max);

        const std::uint32_t thisBinFrames = (i + 1 == srcBins.size()) ? trailingFrameCount : framesPerSrcBin;
        const std::uint64_t binStart      = static_cast<std::uint64_t>(i) * framesPerSrcBin;
        const std::uint64_t binEnd        = binStart + thisBinFrames;

        const std::uint64_t overlapStart  = std::max(binStart, frameStart);
        const std::uint64_t overlapEnd    = std::min(binEnd, frameEnd);
        const std::uint64_t overlapFrames = overlapEnd > overlapStart ? overlapEnd - overlapStart : 0;

        sumSqWeighted += static_cast<double>(b.rms) * static_cast<double>(b.rms) * static_cast<double>(overlapFrames);
        coveredFrames += overlapFrames;
    }

    out.min     = lo;
    out.max     = hi;
    out.rms     = coveredFrames > 0 ? static_cast<Sample>(std::sqrt(sumSqWeighted / static_cast<double>(coveredFrames))) : 0.0f;
    out.absPeak = std::max(-lo, hi);
    return out;
}

void WaveformPyramid::reset(ChannelIndex channelCount) {
    m_channels.assign(channelCount, ChannelState{});
}

ChannelIndex WaveformPyramid::channelCount() const noexcept {
    return static_cast<ChannelIndex>(m_channels.size());
}

void WaveformPyramid::cascade(ChannelState& state) {
    std::uint32_t level = 0;
    while (true) {
        if (level + 1 >= state.levels.size()) {
            if (state.levels[level].size() < 2) {
                break;  // not enough data yet to start the next level
            }
            state.levels.emplace_back();
            state.trailing.push_back(0);
        }

        auto&       lower     = state.levels[level];
        auto&       upper     = state.levels[level + 1];
        const auto  framesLvl = framesPerBin(level);
        bool        progressed = false;

        while (lower.size() >= 2 * (upper.size() + 1)) {
            const std::size_t idxA = 2 * upper.size();
            const std::size_t idxB = idxA + 1;
            const std::uint32_t framesA = (idxA + 1 == lower.size()) ? state.trailing[level] : framesLvl;
            const std::uint32_t framesB = (idxB + 1 == lower.size()) ? state.trailing[level] : framesLvl;

            upper.push_back(fold(lower[idxA], framesA, lower[idxB], framesB));
            state.trailing[level + 1] = framesA + framesB;
            progressed                = true;
        }

        if (!progressed) {
            break;
        }
        ++level;
    }
}

void WaveformPyramid::appendLevel0Bins(ChannelIndex ch, std::span<const WaveformBin> bins, std::size_t framesCovered) {
    if (ch >= m_channels.size() || bins.empty()) {
        return;
    }
    ChannelState& state = m_channels[ch];
    if (state.levels.empty()) {
        state.levels.emplace_back();
        state.trailing.push_back(0);
    }

    auto& level0 = state.levels[0];
    level0.insert(level0.end(), bins.begin(), bins.end());

    // Every bin covers kBaseBinFrames frames except possibly the very last one appended, which may
    // be short (only ever the last bin of the whole channel — M04's chunk-alignment invariant).
    const auto lastBinFrames =
        framesCovered - (bins.size() - 1) * kBaseBinFrames;
    state.trailing[0] = static_cast<std::uint32_t>(lastBinFrames);

    cascade(state);
}

void WaveformPyramid::finalize() {
    for (ChannelState& state : m_channels) {
        if (state.finalized || state.levels.empty()) {
            state.finalized = true;
            continue;
        }

        // Drop levels built beyond the point where a level already has <= 32 bins (M05 "Build
        // levels until a level has <= 32 bins"); levels below that threshold are geometrically
        // negligible to keep anyway, but trimming keeps levelCount() matching the doc's table.
        std::size_t usableLevels = state.levels.size();
        for (std::size_t l = 0; l < state.levels.size(); ++l) {
            if (state.levels[l].size() <= 32) {
                usableLevels = l + 1;
                break;
            }
        }
        state.levels.resize(usableLevels);
        state.trailing.resize(usableLevels);

        state.offsets.assign(usableLevels + 1, 0);
        std::size_t total = 0;
        for (std::size_t l = 0; l < usableLevels; ++l) {
            state.offsets[l] = static_cast<std::uint32_t>(total);
            total += state.levels[l].size();
        }
        state.offsets[usableLevels] = static_cast<std::uint32_t>(total);

        state.packed.resize(total);
        for (std::size_t l = 0; l < usableLevels; ++l) {
            std::copy(state.levels[l].begin(), state.levels[l].end(), state.packed.begin() + state.offsets[l]);
        }

        state.levels.clear();
        state.levels.shrink_to_fit();
        state.finalized = true;
    }
}

std::uint32_t WaveformPyramid::levelCount(ChannelIndex ch) const noexcept {
    if (ch >= m_channels.size()) {
        return 0;
    }
    const ChannelState& state = m_channels[ch];
    return static_cast<std::uint32_t>(state.finalized ? state.offsets.empty() ? 0 : state.offsets.size() - 1
                                                        : state.levels.size());
}

std::uint32_t WaveformPyramid::maxLevel(ChannelIndex ch) const noexcept {
    const std::uint32_t count = levelCount(ch);
    return count == 0 ? 0 : count - 1;
}

std::span<const WaveformBin> WaveformPyramid::level(ChannelIndex ch, std::uint32_t level) const noexcept {
    if (ch >= m_channels.size()) {
        return {};
    }
    const ChannelState& state = m_channels[ch];
    if (state.finalized) {
        if (level + 1 >= state.offsets.size()) {
            return {};
        }
        return std::span<const WaveformBin>(state.packed.data() + state.offsets[level],
                                             state.offsets[level + 1] - state.offsets[level]);
    }
    if (level >= state.levels.size()) {
        return {};
    }
    return state.levels[level];
}

std::uint32_t WaveformPyramid::trailingFrameCount(ChannelIndex ch, std::uint32_t level) const noexcept {
    if (ch >= m_channels.size() || level >= m_channels[ch].trailing.size()) {
        return 0;
    }
    return m_channels[ch].trailing[level];
}

std::size_t WaveformPyramid::totalBinCount(ChannelIndex ch) const noexcept {
    if (ch >= m_channels.size()) {
        return 0;
    }
    const ChannelState& state = m_channels[ch];
    if (state.finalized) {
        return state.packed.size();
    }
    std::size_t total = 0;
    for (const auto& lvl : state.levels) {
        total += lvl.size();
    }
    return total;
}

}  // namespace aud::waveform

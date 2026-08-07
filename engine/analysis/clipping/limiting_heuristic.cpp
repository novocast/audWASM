#include "limiting_heuristic.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aud::clipping {

namespace {
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double linearToDb(double linear) noexcept {
    return linear > 0.0 ? 20.0 * std::log10(linear) : kNegInf;
}
}  // namespace

std::size_t LimitingHeuristicAccumulator::bucketForDbfs(double dbfs) noexcept {
    if (!(dbfs > kHistogramFloorDb)) return 0;  // NaN/-inf/below-floor all land in the floor bucket
    double clamped = std::min(dbfs, 0.0);
    const auto bucket =
        static_cast<std::size_t>((clamped - kHistogramFloorDb) / kBucketWidthDb + 0.5);
    return std::min(bucket, bucketCount() - 1);
}

void LimitingHeuristicAccumulator::begin(ChannelIndex channels) {
    m_channels = channels;
    m_histogram.assign(channels, std::vector<std::uint64_t>(bucketCount(), 0));
    m_prevValue.assign(channels, Sample{0});
    m_hasPrev.assign(channels, false);
    m_runLength.assign(channels, 0);
    m_plateauRunTotalLength = 0;
    m_plateauRunCount       = 0;
    m_samplesConsidered     = 0;
}

void LimitingHeuristicAccumulator::flushPlateau(ChannelIndex ch) noexcept {
    if (m_runLength[ch] >= 2) {
        m_plateauRunTotalLength += m_runLength[ch];
        ++m_plateauRunCount;
    }
    m_runLength[ch] = 0;
}

void LimitingHeuristicAccumulator::process(ChannelIndex channel, std::span<const Sample> samples) noexcept {
    if (channel >= m_channels) return;
    auto& hist = m_histogram[channel];

    for (Sample s : samples) {
        const double mag = static_cast<double>(std::fabs(s));
        ++hist[bucketForDbfs(linearToDb(mag))];
        ++m_samplesConsidered;

        if (m_hasPrev[channel] && s == m_prevValue[channel]) {
            ++m_runLength[channel];
        } else {
            flushPlateau(channel);
            m_runLength[channel] = 1;
        }
        m_prevValue[channel] = s;
        m_hasPrev[channel]   = true;
    }
}

LimitingHeuristicResult LimitingHeuristicAccumulator::finish(std::span<const double> peakLinearPerChannel) const {
    LimitingHeuristicResult result;
    result.samplesConsidered = m_samplesConsidered;

    std::uint64_t nearPeakSamples = 0;
    for (ChannelIndex ch = 0; ch < m_channels && ch < peakLinearPerChannel.size(); ++ch) {
        const double peakDbfs = linearToDb(peakLinearPerChannel[ch]);
        if (!(peakDbfs > kHistogramFloorDb)) continue;  // silent channel: nothing is "near peak"

        const std::size_t hiBucket = bucketForDbfs(peakDbfs);                     // at the peak
        const std::size_t loBucket = bucketForDbfs(peakDbfs - kFlatTopWindowDb);  // kFlatTopWindowDb below it
        const auto& hist = m_histogram[ch];
        for (std::size_t b = loBucket; b <= hiBucket && b < hist.size(); ++b) {
            nearPeakSamples += hist[b];
        }
    }

    result.flatTopRatio = m_samplesConsidered > 0
                               ? static_cast<double>(nearPeakSamples) / static_cast<double>(m_samplesConsidered)
                               : 0.0;

    // The very last in-progress run per channel is never flushed by process() (there's no "next,
    // different sample" to trigger it) — fold it in here without mutating accumulator state, since
    // finish() is logically const (mirrors BlockAccumulator::finish()'s tail-flush convention).
    std::uint64_t totalLength = m_plateauRunTotalLength;
    std::uint64_t count       = m_plateauRunCount;
    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        if (m_runLength[ch] >= 2) {
            totalLength += m_runLength[ch];
            ++count;
        }
    }
    result.meanPlateauLength = count > 0 ? static_cast<double>(totalLength) / static_cast<double>(count) : 0.0;

    result.heavyLimitingLikely = result.flatTopRatio >= kHeavyLimitingFlatTopThreshold;

    return result;
}

}  // namespace aud::clipping

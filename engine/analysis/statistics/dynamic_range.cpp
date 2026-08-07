#include "dynamic_range.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>

namespace aud::statistics {

namespace {
double linearToDb(double linear) {
    return linear <= 0.0 ? -std::numeric_limits<double>::infinity() : 20.0 * std::log10(linear);
}
}  // namespace

double computeDynamicRangeDb(const std::vector<double>& blockRms, const std::vector<double>& blockPeaks) {
    const std::size_t blockCount = std::min(blockRms.size(), blockPeaks.size());
    if (blockCount == 0) return 0.0;

    std::vector<std::size_t> order(blockCount);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return blockRms[a] > blockRms[b]; });

    // Top 20% by RMS, at least one block (M09: "use the top 20% of blocks by RMS").
    const std::size_t topCount = std::max<std::size_t>(1, blockCount / 5);

    double sumPeakDb    = 0.0;
    double sumRmsSquare = 0.0;
    for (std::size_t i = 0; i < topCount; ++i) {
        const std::size_t idx = order[i];
        sumPeakDb    += linearToDb(blockPeaks[idx]);
        sumRmsSquare += blockRms[idx] * blockRms[idx];
    }

    const double meanPeakDb = sumPeakDb / static_cast<double>(topCount);
    // Power-domain average of the subset's RMS values, not a naive mean of dB — same "average in
    // the power domain" rule M08's gating uses (M09 numerical care, applied to this convention too).
    const double rmsOfSubsetDb = linearToDb(std::sqrt(sumRmsSquare / static_cast<double>(topCount)));

    return meanPeakDb - rmsOfSubsetDb;
}

}  // namespace aud::statistics

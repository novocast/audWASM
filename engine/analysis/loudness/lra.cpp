#include "lra.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "gating.hpp"

namespace aud::loudness {

namespace {

constexpr double kAbsoluteGateLufs    = -70.0;
constexpr double kRelativeGateOffsetLu = 20.0;  // LRA's own relative gate — distinct from I's 10 LU

constexpr std::size_t kPairwiseBaseCase = 128;

double pairwiseSum(std::span<const double> values) noexcept {
    const std::size_t n = values.size();
    if (n == 0) return 0.0;
    if (n <= kPairwiseBaseCase) {
        double sum = 0.0;
        for (double v : values) sum += v;
        return sum;
    }
    const std::size_t half = n / 2;
    return pairwiseSum(values.first(half)) + pairwiseSum(values.subspan(half));
}

// Nearest-rank percentile, matching EBU Tech 3342 §5's own published MATLAB reference exactly:
// stl_sorted_vec(round((n-1)*PRC/100 + 1)) in MATLAB's 1-based indexing is
// sortedAscending[round((n-1)*percentile/100)] here (0-based) — round() commutes with the +1/-1
// shift. This is nearest-rank, *not* linearly interpolated between order statistics; an earlier
// version of this function interpolated, which is a defensible general percentile convention but
// disagrees with the specific algorithm the compliance tests are written against.
double nearestRankPercentile(const std::vector<double>& sortedAscending, double percentile) noexcept {
    const std::size_t n = sortedAscending.size();
    if (n == 0) return std::numeric_limits<double>::quiet_NaN();

    const double position = (percentile / 100.0) * static_cast<double>(n - 1);
    auto         index    = static_cast<std::size_t>(std::llround(position));
    index                 = std::min(index, n - 1);
    return sortedAscending[index];
}

}  // namespace

double computeLoudnessRange(std::span<const double> shortTermMeanSquares) {
    const double absoluteThresholdZ = meanSquareFromLoudness(kAbsoluteGateLufs);

    std::vector<double> stage1;
    stage1.reserve(shortTermMeanSquares.size());
    for (double z : shortTermMeanSquares) {
        if (z >= absoluteThresholdZ) stage1.push_back(z);
    }
    if (stage1.empty()) return std::numeric_limits<double>::quiet_NaN();

    const double relativeLoudness   = loudnessFromMeanSquare(pairwiseSum(stage1) / static_cast<double>(stage1.size()));
    const double relativeThresholdZ = meanSquareFromLoudness(relativeLoudness - kRelativeGateOffsetLu);

    std::vector<double> gatedLoudnesses;
    gatedLoudnesses.reserve(stage1.size());
    for (double z : stage1) {
        if (z >= relativeThresholdZ) gatedLoudnesses.push_back(loudnessFromMeanSquare(z));
    }
    if (gatedLoudnesses.empty()) return std::numeric_limits<double>::quiet_NaN();

    std::sort(gatedLoudnesses.begin(), gatedLoudnesses.end());

    // Tech 3342: LRA = L(95th percentile) - L(10th percentile). Asymmetric on purpose.
    return nearestRankPercentile(gatedLoudnesses, 95.0) - nearestRankPercentile(gatedLoudnesses, 10.0);
}

}  // namespace aud::loudness

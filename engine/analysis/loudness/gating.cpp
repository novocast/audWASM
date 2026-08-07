#include "gating.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace aud::loudness {

namespace {

constexpr std::size_t kPairwiseBaseCase = 128;
constexpr double      kAbsoluteGateLufs = -70.0;
constexpr double      kRelativeGateOffsetLu = 10.0;

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

double meanOf(std::span<const double> values) noexcept {
    if (values.empty()) return 0.0;
    return pairwiseSum(values) / static_cast<double>(values.size());
}

}  // namespace

double loudnessFromMeanSquare(double weightedMeanSquare) noexcept {
    if (weightedMeanSquare <= 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    return -0.691 + 10.0 * std::log10(weightedMeanSquare);
}

double meanSquareFromLoudness(double lufs) noexcept {
    return std::pow(10.0, (lufs + 0.691) / 10.0);
}

GateResult gateIntegratedLoudness(std::span<const double> momentaryMeanSquares) {
    const double absoluteThresholdZ = meanSquareFromLoudness(kAbsoluteGateLufs);

    std::vector<double> stage1;
    stage1.reserve(momentaryMeanSquares.size());
    for (double z : momentaryMeanSquares) {
        if (z >= absoluteThresholdZ) stage1.push_back(z);
    }
    if (stage1.empty()) {
        return GateResult{std::numeric_limits<double>::quiet_NaN()};
    }

    const double relativeLoudness  = loudnessFromMeanSquare(meanOf(stage1));
    const double relativeThresholdZ = meanSquareFromLoudness(relativeLoudness - kRelativeGateOffsetLu);

    std::vector<double> stage2;
    stage2.reserve(stage1.size());
    for (double z : stage1) {
        if (z >= relativeThresholdZ) stage2.push_back(z);
    }
    if (stage2.empty()) {
        return GateResult{std::numeric_limits<double>::quiet_NaN()};
    }

    return GateResult{loudnessFromMeanSquare(meanOf(stage2))};
}

}  // namespace aud::loudness

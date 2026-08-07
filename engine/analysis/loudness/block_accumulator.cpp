#include "block_accumulator.hpp"

#include <algorithm>
#include <cmath>

namespace aud::loudness {

namespace {

constexpr std::size_t kPairwiseBaseCase = 128;

// Pairwise sum-of-squares over doubles (the util/accumulate.hpp helpers are fixed to
// std::span<const Sample>/float; K-weighted samples here are already double). Same rationale as
// M08's risk table: naive left-to-right accumulation over a long integration loses precision.
double pairwiseSumSquares(std::span<const double> values) noexcept {
    const std::size_t n = values.size();
    if (n == 0) return 0.0;
    if (n <= kPairwiseBaseCase) {
        double sum = 0.0;
        for (double v : values) sum += v * v;
        return sum;
    }
    const std::size_t half = n / 2;
    return pairwiseSumSquares(values.first(half)) + pairwiseSumSquares(values.subspan(half));
}

}  // namespace

void BlockAccumulator::begin(SampleRate sampleRate, std::span<const double> channelWeights) {
    m_sampleRate = sampleRate;
    m_weights.assign(channelWeights.begin(), channelWeights.end());

    m_subBlockFrames = static_cast<std::size_t>(std::llround(static_cast<double>(sampleRate) * 0.1));
    if (m_subBlockFrames == 0) m_subBlockFrames = 1;

    m_carry.assign(m_weights.size(), {});
    for (auto& carry : m_carry) carry.reserve(m_subBlockFrames);
}

void BlockAccumulator::process(std::span<const std::span<const double>> kWeightedChannels,
                                const SubBlockCallback& onSubBlock) {
    if (kWeightedChannels.empty() || m_subBlockFrames == 0) return;

    const std::size_t frameCount = kWeightedChannels[0].size();
    std::size_t       frameOffset = 0;

    while (frameOffset < frameCount) {
        const std::size_t carried   = m_carry.empty() ? 0 : m_carry[0].size();
        const std::size_t needed    = m_subBlockFrames - carried;
        const std::size_t available = frameCount - frameOffset;
        const std::size_t take      = std::min(needed, available);

        for (std::size_t c = 0; c < m_weights.size() && c < kWeightedChannels.size(); ++c) {
            auto&       carry = m_carry[c];
            const auto& src   = kWeightedChannels[c];
            carry.insert(carry.end(), src.begin() + frameOffset, src.begin() + frameOffset + take);
        }
        frameOffset += take;

        if (!m_carry.empty() && m_carry[0].size() == m_subBlockFrames) {
            double combined = 0.0;
            for (std::size_t c = 0; c < m_weights.size(); ++c) {
                if (m_weights[c] == 0.0) continue;  // LFE (or any excluded lane) never contributes
                const double meanSquare = pairwiseSumSquares(m_carry[c]) / static_cast<double>(m_subBlockFrames);
                combined += m_weights[c] * meanSquare;
            }
            onSubBlock(combined);
            for (auto& carry : m_carry) carry.clear();
        }
    }
}

}  // namespace aud::loudness

#include "stereo.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aud::statistics {

namespace {
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double correlationFromSums(double n, double sumL, double sumR, double sumLL, double sumRR, double sumLR) {
    if (n <= 0.0) return 0.0;
    const double numerator   = n * sumLR - sumL * sumR;
    const double denomLeft   = n * sumLL - sumL * sumL;
    const double denomRight  = n * sumRR - sumR * sumR;
    const double denom       = denomLeft * denomRight;
    if (denom <= 0.0) return 0.0;  // one (or both) channels are silent/constant — no correlation to report
    return numerator / std::sqrt(denom);
}
}  // namespace

void StereoAccumulator::begin(SampleRate sampleRate) noexcept {
    m_n     = 0;
    m_sumLR = 0.0;

    m_windowFrames = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(sampleRate) / 20);  // 50ms
    m_windowCount  = 0;
    m_windowSumL = m_windowSumR = m_windowSumLL = m_windowSumRR = m_windowSumLR = 0.0;
    m_correlationSeries.clear();
}

void StereoAccumulator::process(std::span<const Sample> left, std::span<const Sample> right) noexcept {
    const std::size_t n = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < n; ++i) {
        const double l = static_cast<double>(left[i]);
        const double r = static_cast<double>(right[i]);

        m_sumLR += l * r;
        ++m_n;

        m_windowSumL  += l;
        m_windowSumR  += r;
        m_windowSumLL += l * l;
        m_windowSumRR += r * r;
        m_windowSumLR += l * r;
        ++m_windowCount;

        if (m_windowCount >= m_windowFrames) {
            const double wc = static_cast<double>(m_windowCount);
            m_correlationSeries.push_back(static_cast<float>(
                correlationFromSums(wc, m_windowSumL, m_windowSumR, m_windowSumLL, m_windowSumRR, m_windowSumLR)));
            m_windowCount = 0;
            m_windowSumL = m_windowSumR = m_windowSumLL = m_windowSumRR = m_windowSumLR = 0.0;
        }
    }
}

void StereoAccumulator::finish() noexcept {
    if (m_windowCount > 0) {
        const double wc = static_cast<double>(m_windowCount);
        m_correlationSeries.push_back(static_cast<float>(
            correlationFromSums(wc, m_windowSumL, m_windowSumR, m_windowSumLL, m_windowSumRR, m_windowSumLR)));
        m_windowCount = 0;
    }
}

StereoStatistics StereoAccumulator::computeStatistics(double meanL, double varL, double sumSqL, double meanR,
                                                        double varR, double sumSqR) const {
    StereoStatistics stats;
    stats.correlationSeries = m_correlationSeries;

    const double n = static_cast<double>(m_n);
    if (n <= 0.0) {
        return stats;
    }

    // DC-removed global correlation: Cov(L, R) / sqrt(Var(L) * Var(R)).
    const double covariance = m_sumLR / n - meanL * meanR;
    const double denom      = varL * varR;
    stats.correlation = denom <= 0.0 ? 0.0 : covariance / std::sqrt(denom);

    const double rmsL = std::sqrt(sumSqL / n);
    const double rmsR = std::sqrt(sumSqR / n);
    stats.balanceDb = rmsL <= 0.0 ? (rmsR <= 0.0 ? 0.0 : std::numeric_limits<double>::infinity())
                                  : 20.0 * std::log10(rmsR / rmsL);

    // Mono-compatibility: energy of (L+R)/2 vs the average energy of L and R alone (M09: "a more
    // actionable statement than r = 0.31").
    const double energyMid    = (sumSqL + 2.0 * m_sumLR + sumSqR) / (4.0 * n);
    const double energyStereo = (sumSqL + sumSqR) / (2.0 * n);
    stats.monoCompatibilityDb = energyStereo <= 0.0 ? 0.0
                              : energyMid <= 0.0     ? kNegInf
                                                      : 10.0 * std::log10(energyMid / energyStereo);

    return stats;
}

}  // namespace aud::statistics

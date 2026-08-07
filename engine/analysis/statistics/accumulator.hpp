#pragma once

// Streaming single-pass per-channel accumulator: peak (+frame), min/max, DC, RMS/variance,
// zero-crossing rate, amplitude histogram, and the 50ms windowed RMS series — everything
// StatisticsAnalyzer needs, gathered without re-reading a chunk once it's out of cache (M09
// "Single pass, shared with everything else"). Cross-channel state (Sigma L*R for stereo
// correlation) lives one level up in StatisticsAnalyzer, since it needs two channels at once.
//
// Numerical care (M09 "Numerical care"): every sum here is either a pairwise reduction within a
// chunk (engine/util/accumulate.hpp) or a plain accumulation of a small number of already-precise
// chunk-partial results — never a naive running sum over the whole file's individual samples.
// Variance uses the shifted-sum-of-squares identity, not naive E[x^2] - E[x]^2, so a DC-offset
// file (which M12 cares about) doesn't catastrophically cancel.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "../../util/accumulate.hpp"
#include "../../util/audio_types.hpp"
#include "histogram.hpp"

namespace aud::statistics {

// One 50ms (configurable) window's worth of accumulated RMS input, used both for the exposed
// windowed RMS series and internally for the 3s DR blocks (dynamic_range.hpp derives its blocks
// from the same accumulation, per M09 "derive 3s blocks from those").
struct WindowAccumulator {
    double        sumSquares = 0.0;
    std::uint64_t count       = 0;
    bool          allZero     = true;  // digital-silence flag (M10): true until a nonzero sample arrives

    void reset() noexcept { sumSquares = 0.0; count = 0; allZero = true; }

    [[nodiscard]] double rms() const noexcept {
        return count == 0 ? 0.0 : std::sqrt(sumSquares / static_cast<double>(count));
    }
};

class ChannelAccumulator {
public:
    // `sampleRate` fixes the window sizes; must be called once before process().
    void begin(SampleRate sampleRate) noexcept {
        m_sampleRate     = sampleRate;
        m_windowFrames   = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(sampleRate) / 20);  // 50ms
        m_blockFrames    = m_windowFrames * 60;                                                       // 3s

        m_n            = 0;
        m_sumX         = 0.0;
        m_sumXShiftSq  = 0.0;
        m_sumXSquares  = 0.0;
        m_sumAbs       = 0.0;
        m_hasShift     = false;
        m_shift        = 0.0;
        m_min          = std::numeric_limits<double>::infinity();
        m_max          = -std::numeric_limits<double>::infinity();
        m_peak         = 0.0;
        m_peakFrame    = kNoFrame;
        m_zeroCrossings = 0;
        m_hasPrevSample = false;
        m_prevSample    = 0.0;
        m_window.reset();
        m_rmsSeries.clear();
        m_allZeroSeries.clear();
        m_currentBlock.reset();
        m_blockPeak = 0.0;
        m_blockRms.clear();
        m_blockPeaks.clear();
        m_histogram = AmplitudeHistogram{};
    }

    // `startFrame` is this chunk's global frame offset (ChunkView::startFrame), used only to stamp
    // the peak's frame index.
    void process(std::span<const Sample> samples, FrameIndex startFrame) noexcept {
        if (samples.empty()) return;

        if (!m_hasShift) {
            m_shift    = static_cast<double>(samples[0]);
            m_hasShift = true;
        }

        m_sumX        += pairwiseSum<double>(samples);
        m_sumXSquares += pairwiseSumSquares<double>(samples);
        m_sumXShiftSq += pairwiseSumSquaresShifted<double>(samples, m_shift);
        m_sumAbs      += pairwiseSumAbs<double>(samples);
        m_n           += samples.size();
        m_histogram.addRange(samples);

        for (std::size_t i = 0; i < samples.size(); ++i) {
            const double v   = static_cast<double>(samples[i]);
            const double av  = v < 0.0 ? -v : v;

            if (v < m_min) m_min = v;
            if (v > m_max) m_max = v;
            if (av > m_peak) {
                m_peak      = av;
                m_peakFrame = startFrame + static_cast<FrameIndex>(i);
            }

            if (m_hasPrevSample && v != 0.0 && m_prevSample != 0.0 && (m_prevSample < 0.0) != (v < 0.0)) {
                ++m_zeroCrossings;
            }
            if (v != 0.0) {
                m_hasPrevSample = true;
                m_prevSample    = v;
            }

            pushWindow(v * v, av, v == 0.0);
        }
    }

    void finish() noexcept {
        flushWindow();
        flushBlock();
    }

    [[nodiscard]] std::uint64_t sampleCount() const noexcept { return m_n; }
    [[nodiscard]] double        mean() const noexcept { return m_n == 0 ? 0.0 : m_sumX / static_cast<double>(m_n); }
    [[nodiscard]] double        sumX() const noexcept { return m_sumX; }
    [[nodiscard]] double        sumXSquares() const noexcept { return m_sumXSquares; }
    [[nodiscard]] double        sumAbs() const noexcept { return m_sumAbs; }
    [[nodiscard]] double        minValue() const noexcept { return m_n == 0 ? 0.0 : m_min; }
    [[nodiscard]] double        maxValue() const noexcept { return m_n == 0 ? 0.0 : m_max; }
    [[nodiscard]] double        peak() const noexcept { return m_peak; }
    [[nodiscard]] FrameIndex    peakFrame() const noexcept { return m_peakFrame; }
    [[nodiscard]] std::uint64_t zeroCrossings() const noexcept { return m_zeroCrossings; }

    // Population variance, via the shift-invariant identity: Var(x) = E[(x-K)^2] - E[x-K]^2 for
    // any constant K. E[x-K] is derived from the exact mean (sumX/n - K) rather than accumulated
    // separately, since that's already an exact-enough quantity at this point.
    [[nodiscard]] double variance() const noexcept {
        if (m_n == 0) return 0.0;
        const double n            = static_cast<double>(m_n);
        const double meanShifted  = mean() - m_shift;
        const double meanSqShifted = m_sumXShiftSq / n;
        double       v            = meanSqShifted - meanShifted * meanShifted;
        return v < 0.0 ? 0.0 : v;  // guard against a sliver of negative from rounding
    }

    [[nodiscard]] double stdDev() const noexcept { return std::sqrt(variance()); }

    [[nodiscard]] double rms() const noexcept {
        return m_n == 0 ? 0.0 : std::sqrt(m_sumXSquares / static_cast<double>(m_n));
    }

    [[nodiscard]] const AmplitudeHistogram& histogram() const noexcept { return m_histogram; }

    // 50ms-resolution RMS series (linear, not dB) — shared with M10/M18 per M09's "no second pass".
    [[nodiscard]] const std::vector<float>& rmsSeries() const noexcept { return m_rmsSeries; }

    // Same grid as rmsSeries(): 1 if every sample in that window was exactly zero (M10's
    // "digital silence" flag), 0 otherwise. Computed alongside the RMS series at no extra pass
    // cost (M10: "a cheap flag per window during M09's pass").
    [[nodiscard]] const std::vector<std::uint8_t>& allZeroSeries() const noexcept { return m_allZeroSeries; }

    // 3s-block RMS/peak pairs, derived from the same single pass, feeding dynamic_range.hpp.
    [[nodiscard]] const std::vector<double>& blockRms() const noexcept { return m_blockRms; }
    [[nodiscard]] const std::vector<double>& blockPeaks() const noexcept { return m_blockPeaks; }

    [[nodiscard]] SampleRate sampleRate() const noexcept { return m_sampleRate; }

private:
    void pushWindow(double squared, double absValue, bool sampleIsZero) noexcept {
        m_window.sumSquares += squared;
        ++m_window.count;
        if (!sampleIsZero) m_window.allZero = false;
        if (absValue > m_blockPeak) m_blockPeak = absValue;
        m_currentBlock.sumSquares += squared;
        ++m_currentBlock.count;

        if (m_window.count >= m_windowFrames) flushWindow();
        if (m_currentBlock.count >= m_blockFrames) flushBlock();
    }

    void flushWindow() noexcept {
        if (m_window.count == 0) return;
        m_rmsSeries.push_back(static_cast<float>(m_window.rms()));
        m_allZeroSeries.push_back(m_window.allZero ? 1 : 0);
        m_window.reset();
    }

    void flushBlock() noexcept {
        if (m_currentBlock.count == 0) return;
        m_blockRms.push_back(m_currentBlock.rms());
        m_blockPeaks.push_back(m_blockPeak);
        m_currentBlock.reset();
        m_blockPeak = 0.0;
    }

    SampleRate    m_sampleRate   = 0;
    std::uint64_t m_windowFrames = 0;
    std::uint64_t m_blockFrames  = 0;

    std::uint64_t m_n           = 0;
    double        m_sumX        = 0.0;
    double        m_sumXShiftSq = 0.0;
    double        m_sumXSquares = 0.0;
    double        m_sumAbs      = 0.0;
    bool          m_hasShift    = false;
    double        m_shift       = 0.0;

    double     m_min = std::numeric_limits<double>::infinity();
    double     m_max = -std::numeric_limits<double>::infinity();
    double     m_peak      = 0.0;
    FrameIndex m_peakFrame = kNoFrame;

    std::uint64_t m_zeroCrossings  = 0;
    bool          m_hasPrevSample  = false;
    double        m_prevSample     = 0.0;

    WindowAccumulator    m_window;
    std::vector<float>   m_rmsSeries;
    std::vector<std::uint8_t> m_allZeroSeries;

    WindowAccumulator     m_currentBlock;
    double                m_blockPeak = 0.0;
    std::vector<double>   m_blockRms;
    std::vector<double>   m_blockPeaks;

    AmplitudeHistogram m_histogram;
};

}  // namespace aud::statistics

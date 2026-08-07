#include "click_detector.hpp"

#include <algorithm>
#include <cmath>

namespace aud::transients {

namespace {

// Autocorrelation-method Levinson-Durbin. Fills `coeffs` (size order) such that the predictor
// x_hat[n] = sum_{k=1..order} coeffs[k-1] * x[n-k]. Returns false if the window is silent/degenerate
// (R[0] ~ 0), in which case the caller should skip prediction for this window entirely.
bool solveLpc(std::span<const Sample> window, std::size_t order, std::vector<double>& coeffs) {
    std::vector<double> r(order + 1, 0.0);
    for (std::size_t lag = 0; lag <= order; ++lag) {
        double sum = 0.0;
        for (std::size_t i = lag; i < window.size(); ++i) sum += static_cast<double>(window[i]) * window[i - lag];
        r[lag] = sum;
    }
    if (r[0] <= 1e-20) return false;

    std::vector<double> a(order + 1, 0.0);
    double               error = r[0];
    for (std::size_t i = 1; i <= order; ++i) {
        double acc = r[i];
        for (std::size_t j = 1; j < i; ++j) acc -= a[j] * r[i - j];
        const double reflection = error > 1e-20 ? acc / error : 0.0;

        std::vector<double> aPrev = a;
        a[i]                       = reflection;
        for (std::size_t j = 1; j < i; ++j) a[j] = aPrev[j] - reflection * aPrev[i - j];

        error *= (1.0 - reflection * reflection);
        if (error < 1e-20) error = 1e-20;
    }

    coeffs.assign(order, 0.0);
    for (std::size_t k = 1; k <= order; ++k) coeffs[k - 1] = a[k];
    return true;
}

double medianOf(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t mid = values.size() / 2;
    return values.size() % 2 == 0 ? 0.5 * (values[mid - 1] + values[mid]) : values[mid];
}

}  // namespace

std::vector<ClickCandidate> detectClicks(std::span<const Sample> mono, SampleRate sampleRate,
                                          ClickDetectorConfig config) {
    std::vector<ClickCandidate> out;
    if (mono.empty() || sampleRate == 0 || config.lpcOrder == 0) return out;
    if (mono.size() <= config.lpcOrder * 2) return out;

    const std::size_t total = mono.size();
    const std::size_t hop   = std::max<std::size_t>(1, std::min(config.hopSamples, config.windowSamples));

    for (std::size_t fitBegin = 0; fitBegin < total; fitBegin += hop) {
        const std::size_t fitEnd = std::min(total, fitBegin + config.windowSamples);
        if (fitEnd - fitBegin <= config.lpcOrder * 2) continue;

        std::vector<double> coeffs;
        if (!solveLpc(mono.subspan(fitBegin, fitEnd - fitBegin), config.lpcOrder, coeffs)) continue;

        // Residuals over the whole fit window (for stable local statistics) — prediction needs
        // `order` samples of lookback, which may reach slightly before fitBegin; that's fine, it's
        // still in-bounds PCM.
        const std::size_t residualBegin = std::max(fitBegin, config.lpcOrder);
        std::vector<double> residuals;
        residuals.reserve(fitEnd - residualBegin);
        for (std::size_t n = residualBegin; n < fitEnd; ++n) {
            double predicted = 0.0;
            for (std::size_t k = 1; k <= config.lpcOrder; ++k) predicted += coeffs[k - 1] * mono[n - k];
            residuals.push_back(std::fabs(static_cast<double>(mono[n]) - predicted));
        }
        if (residuals.empty()) continue;

        const double median = medianOf(residuals);
        std::vector<double> deviations;
        deviations.reserve(residuals.size());
        for (double v : residuals) deviations.push_back(std::fabs(v - median));
        const double mad = std::max(medianOf(deviations), config.minAbsResidual);

        const double threshold = median + config.madMultiplier * mad;

        // Only test the hop-sized chunk this iteration owns, so overlapping fit windows don't
        // produce duplicate candidates for the same sample.
        const std::size_t testBegin = std::max(residualBegin, fitBegin);
        const std::size_t testEnd   = std::min(fitEnd, fitBegin + hop);
        for (std::size_t n = testBegin; n < testEnd; ++n) {
            const double residual = residuals[n - residualBegin];
            if (residual > threshold && residual > config.minAbsResidual) {
                ClickCandidate candidate;
                candidate.frame         = static_cast<FrameIndex>(n);
                candidate.residualRatio = static_cast<float>(mad > 0.0 ? residual / mad : 0.0);
                out.push_back(candidate);
            }
        }
    }

    // Merge runs of adjacent flagged samples (a single click typically spans a handful of samples)
    // into one candidate at the strongest point.
    std::vector<ClickCandidate> merged;
    for (std::size_t i = 0; i < out.size();) {
        std::size_t j = i;
        ClickCandidate best = out[i];
        while (j + 1 < out.size() && out[j + 1].frame - out[j].frame <= 2) {
            ++j;
            if (out[j].residualRatio > best.residualRatio) best = out[j];
        }
        merged.push_back(best);
        i = j + 1;
    }

    return merged;
}

std::vector<ClickCandidate> rejectOnsetCoincidences(std::vector<ClickCandidate> candidates, SampleRate sampleRate,
                                                     std::span<const double> onsetTimesSeconds, double toleranceMs) {
    if (onsetTimesSeconds.empty() || sampleRate == 0) return candidates;

    const double toleranceFrames = toleranceMs * 0.001 * static_cast<double>(sampleRate);
    std::vector<ClickCandidate> out;
    out.reserve(candidates.size());
    for (const ClickCandidate& candidate : candidates) {
        const double frame     = static_cast<double>(candidate.frame);
        bool         coincides = false;
        for (double onsetSeconds : onsetTimesSeconds) {
            const double onsetFrame = onsetSeconds * static_cast<double>(sampleRate);
            if (std::fabs(onsetFrame - frame) <= toleranceFrames) {
                coincides = true;
                break;
            }
        }
        if (!coincides) out.push_back(candidate);
    }
    return out;
}

}  // namespace aud::transients

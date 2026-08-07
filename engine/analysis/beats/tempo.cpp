#include "tempo.hpp"

#include <algorithm>
#include <cmath>

namespace aud::beats {

namespace {
constexpr double kLog2 = 0.6931471805599453;

double logNormalPrior(double bpm, double centerBpm, double sigmaOctaves) noexcept {
    const double octaves = std::log(bpm / centerBpm) / kLog2;
    const double z       = octaves / sigmaOctaves;
    return std::exp(-0.5 * z * z);
}

// Autocorrelation of `odf` at integer lag `lag` frames, biased-normalised by the overlap length so
// long/short lags are comparable.
double autocorrelationAt(std::span<const float> odf, std::size_t lag) {
    if (lag == 0 || lag >= odf.size()) return 0.0;
    double sum = 0.0;
    const std::size_t count = odf.size() - lag;
    for (std::size_t i = 0; i < count; ++i) {
        sum += static_cast<double>(odf[i]) * static_cast<double>(odf[i + lag]);
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

// Harmonic sum / comb-filter step: a tempo's own lag plus its integer multiples (2x, 3x period ==
// half, third the BPM) reinforce it, per the doc's "so that a tempo and its multiples reinforce."
double harmonicSumScore(std::span<const float> odf, std::size_t lag, std::size_t harmonics) {
    double score = 0.0;
    for (std::size_t h = 1; h <= harmonics; ++h) {
        score += autocorrelationAt(odf, lag * h) / static_cast<double>(h);
    }
    return score;
}

}  // namespace

TempoEstimate estimateTempo(std::span<const float> odf, double hopSeconds, TempoConfig config) {
    TempoEstimate out;
    if (odf.size() < 8 || hopSeconds <= 0.0) return out;

    const std::size_t minLag = std::max<std::size_t>(1, static_cast<std::size_t>(60.0 / config.maxBpm / hopSeconds));
    const std::size_t maxLag = std::min(odf.size() - 1, static_cast<std::size_t>(60.0 / config.minBpm / hopSeconds));
    if (minLag >= maxLag) return out;

    std::vector<double> rawScore(maxLag - minLag + 1, 0.0);
    std::vector<double> weightedScore(rawScore.size(), 0.0);

    for (std::size_t lag = minLag; lag <= maxLag; ++lag) {
        const double bpm   = 60.0 / (static_cast<double>(lag) * hopSeconds);
        const double raw   = harmonicSumScore(odf, lag, 3);
        const double prior = logNormalPrior(bpm, config.priorCenterBpm, config.priorSigmaOctaves);

        rawScore[lag - minLag]      = raw;
        weightedScore[lag - minLag] = raw * prior;
    }

    // Local maxima of the *raw* (pre-prior) score, one candidate per maximum, refined to sub-lag
    // precision by quadratic interpolation (same technique as fft/peak_interp.hpp) — integer-lag
    // resolution alone can be a full BPM off at typical hop sizes, nowhere near the doc's ±0.1 BPM
    // click-track acceptance criterion.
    //
    // Deliberately *not* local maxima of the prior-weighted score: the log-Gaussian prior is a
    // smooth, continuous function of lag, and multiplying it through before peak-picking can erase
    // a real ÷2/×2 bump in the raw autocorrelation entirely (a full octave away from the prior's
    // centre, the prior itself is already decaying steeply, so the weighted curve's local shape
    // there is dominated by the prior, not by the actual periodicity) — which would silently defeat
    // the doc's explicit requirement to report octave alternatives. The prior is applied afterwards,
    // only to rank/select among genuine raw peaks.
    std::vector<TempoCandidate> candidates;
    for (std::size_t i = 0; i < rawScore.size(); ++i) {
        const bool leftOk  = i == 0 || rawScore[i] >= rawScore[i - 1];
        const bool rightOk = i + 1 == rawScore.size() || rawScore[i] >= rawScore[i + 1];
        if (!leftOk || !rightOk) continue;

        double lagRefined = static_cast<double>(minLag + i);
        if (i > 0 && i + 1 < rawScore.size()) {
            const double left = rawScore[i - 1], center = rawScore[i], right = rawScore[i + 1];
            const double denom = left - 2.0 * center + right;
            if (denom != 0.0) lagRefined += 0.5 * (left - right) / denom;
        }

        const double bpm   = 60.0 / (lagRefined * hopSeconds);
        const double prior = logNormalPrior(bpm, config.priorCenterBpm, config.priorSigmaOctaves);
        candidates.push_back(TempoCandidate{bpm, static_cast<float>(rawScore[i] * prior)});
    }
    if (candidates.empty()) return out;

    std::sort(candidates.begin(), candidates.end(),
              [](const TempoCandidate& a, const TempoCandidate& b) { return a.score > b.score; });

    const double bestScore = candidates.front().score;

    out.primaryBpm = candidates.front().bpm;
    for (const auto& c : candidates) {
        if (out.alternatives.size() >= config.maxAlternatives) break;
        if (c.score < bestScore * (1.0 - config.alternativeScoreFraction) && !out.alternatives.empty()) continue;
        out.alternatives.push_back(c);
    }

    // Confidence: peakiness of the weighted-score curve — how far the best candidate stands above
    // the mean, relative to the curve's own spread. Flat/noisy curves (white noise) score low;
    // one dominant lag (a click track) scores high.
    double mean = 0.0;
    for (double s : weightedScore) mean += s;
    mean /= static_cast<double>(weightedScore.size());

    double variance = 0.0;
    for (double s : weightedScore) variance += (s - mean) * (s - mean);
    variance /= static_cast<double>(weightedScore.size());
    const double stddev = std::sqrt(std::max(variance, 1e-12));

    const double zScore   = (bestScore - mean) / stddev;
    out.tempoConfidence    = static_cast<float>(std::clamp(zScore / 8.0, 0.0, 1.0));

    return out;
}

TempoSeriesResult estimateTempoSeries(std::span<const float> odf, double hopSeconds, TempoConfig config) {
    TempoSeriesResult out;
    if (hopSeconds <= 0.0 || odf.empty()) return out;

    const std::size_t windowFrames = std::max<std::size_t>(8, static_cast<std::size_t>(config.windowSeconds / hopSeconds));

    std::vector<double> windowBpms;
    for (std::size_t start = 0; start < odf.size(); start += windowFrames) {
        const std::size_t end = std::min(odf.size(), start + windowFrames);
        if (end - start < 8) break;  // trailing partial window too short to estimate

        const auto estimate = estimateTempo(odf.subspan(start, end - start), hopSeconds, config);
        out.tempoSeries.push_back(static_cast<float>(estimate.primaryBpm));
        if (estimate.primaryBpm > 0.0) windowBpms.push_back(estimate.primaryBpm);
    }

    if (windowBpms.size() < 2) {
        out.tempoIsStable = true;  // not enough windows to call it varying
        return out;
    }

    double mean = 0.0;
    for (double b : windowBpms) mean += b;
    mean /= static_cast<double>(windowBpms.size());

    double variance = 0.0;
    for (double b : windowBpms) variance += (b - mean) * (b - mean);
    variance /= static_cast<double>(windowBpms.size());
    const double stddev = std::sqrt(variance);

    // Coefficient of variation: a programmed track's window-to-window BPM should barely move;
    // a ramp or live performance moves by several percent or more.
    const double coefficientOfVariation = mean > 0.0 ? stddev / mean : 0.0;
    out.tempoIsStable = coefficientOfVariation < 0.04;

    return out;
}

}  // namespace aud::beats

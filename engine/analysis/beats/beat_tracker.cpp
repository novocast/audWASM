#include "beat_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aud::beats {

BeatTrackResult trackBeats(std::span<const float> odf, double hopSeconds, double periodSeconds,
                            BeatTrackerConfig config) {
    BeatTrackResult out;
    if (odf.empty() || hopSeconds <= 0.0 || periodSeconds <= 0.0) return out;

    const double periodFrames = periodSeconds / hopSeconds;
    if (periodFrames < 1.0) return out;

    const std::size_t n = odf.size();
    std::vector<double> cumulativeScore(n, 0.0);
    std::vector<std::ptrdiff_t> backlink(n, -1);

    const std::ptrdiff_t searchLo = -static_cast<std::ptrdiff_t>(std::lround(2.0 * periodFrames));
    const std::ptrdiff_t searchHi = -static_cast<std::ptrdiff_t>(std::lround(0.5 * periodFrames));

    for (std::size_t t = 0; t < n; ++t) {
        double bestScore            = -std::numeric_limits<double>::infinity();
        std::ptrdiff_t bestTau       = -1;

        const std::ptrdiff_t tSigned = static_cast<std::ptrdiff_t>(t);
        for (std::ptrdiff_t delta = searchLo; delta <= searchHi; ++delta) {
            const std::ptrdiff_t tau = tSigned + delta;
            if (tau < 0) continue;

            const double deviation = std::log(static_cast<double>(-delta) / periodFrames);
            const double penalty   = -config.tightness * deviation * deviation;
            const double score      = cumulativeScore[static_cast<std::size_t>(tau)] + penalty;

            if (score > bestScore) {
                bestScore = score;
                bestTau   = tau;
            }
        }

        if (bestTau >= 0) {
            cumulativeScore[t] = static_cast<double>(odf[t]) + std::max(0.0, bestScore);
            backlink[t]         = bestTau;
        } else {
            cumulativeScore[t] = static_cast<double>(odf[t]);
            backlink[t]         = -1;
        }
    }

    // Backtrack from the globally best-scoring frame.
    std::size_t bestEnd = 0;
    for (std::size_t t = 1; t < n; ++t) {
        if (cumulativeScore[t] > cumulativeScore[bestEnd]) bestEnd = t;
    }

    std::vector<std::size_t> beats;
    std::ptrdiff_t cursor = static_cast<std::ptrdiff_t>(bestEnd);
    while (cursor >= 0) {
        beats.push_back(static_cast<std::size_t>(cursor));
        cursor = backlink[static_cast<std::size_t>(cursor)];
    }
    std::reverse(beats.begin(), beats.end());

    out.beatFrames = std::move(beats);
    out.beatStrengths.reserve(out.beatFrames.size());
    for (std::size_t f : out.beatFrames) out.beatStrengths.push_back(odf[f]);

    // Phase confidence: how far the beat-selected ODF values stand above the series' own mean,
    // relative to its spread — a confident phase lands beats on strong peaks, not the noise floor.
    double meanOdf = 0.0;
    for (float v : odf) meanOdf += v;
    meanOdf /= static_cast<double>(n);

    double varOdf = 0.0;
    for (float v : odf) varOdf += (v - meanOdf) * (v - meanOdf);
    varOdf /= static_cast<double>(n);
    const double stddevOdf = std::sqrt(std::max(varOdf, 1e-12));

    double meanBeat = 0.0;
    for (float v : out.beatStrengths) meanBeat += v;
    if (!out.beatStrengths.empty()) meanBeat /= static_cast<double>(out.beatStrengths.size());

    const double z = (meanBeat - meanOdf) / stddevOdf;
    out.phaseConfidence = static_cast<float>(std::clamp(z / 4.0, 0.0, 1.0));

    return out;
}

}  // namespace aud::beats

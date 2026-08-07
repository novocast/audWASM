#include "normalise.hpp"

#include <algorithm>
#include <cmath>

namespace aud::beats {

namespace {
// 1.4826 makes MAD a consistent estimator of stddev for normal-ish data — same constant DC's
// sectional-step classifier uses (analysis/dc/dc_analyzer.cpp) for the same reason.
constexpr double kMadToStddev = 1.4826;
constexpr double kEps         = 1e-6;

double median(std::vector<float>& scratch) {
    std::sort(scratch.begin(), scratch.end());
    return scratch[scratch.size() / 2];
}
}  // namespace

std::vector<float> normaliseOdf(std::span<const float> raw, NormaliseConfig config) {
    std::vector<float> out(raw.size(), 0.0f);
    if (raw.empty() || config.windowFrames == 0) return out;

    std::vector<float> scratch;
    scratch.reserve(config.windowFrames);

    // Centred, not causal: unlike a truly streaming detector, this runs once over the whole
    // retained ODF at finish() (BeatAnalyzer), so there's no reason to only look backward — and a
    // causal window very visibly does the wrong thing on a periodic source (a click track): the
    // trailing window right after each click still contains that click's own decay tail, which
    // asymmetrically drags the local median/MAD and measurably biased onset timing in practice.
    const std::size_t halfWindow = config.windowFrames / 2;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const std::size_t begin = i >= halfWindow ? i - halfWindow : 0;
        const std::size_t end   = std::min(raw.size(), i + halfWindow + 1);
        scratch.assign(raw.begin() + static_cast<std::ptrdiff_t>(begin), raw.begin() + static_cast<std::ptrdiff_t>(end));

        const double med = median(scratch);
        for (float& v : scratch) v = static_cast<float>(std::fabs(static_cast<double>(v) - med));
        const double mad = median(scratch);

        const double denom = kMadToStddev * mad + kEps;
        out[i]              = static_cast<float>((static_cast<double>(raw[i]) - med) / denom);
    }

    return out;
}

}  // namespace aud::beats

#include "peak_pick.hpp"

#include <algorithm>
#include <cmath>

namespace aud::beats {

namespace {

// Quadratic interpolation through three consecutive ODF samples centred on a local maximum —
// same math as fft/peak_interp.hpp's interpolatePeak(), without the log() step (the ODF fed here
// is already log-compressed and normalised upstream; see peak_pick.hpp).
struct OdfPeakEstimate {
    double offset = 0.0;
    double value  = 0.0;
};

OdfPeakEstimate interpolateOdfPeak(std::span<const float> odf, std::size_t i) noexcept {
    const double left   = static_cast<double>(odf[i - 1]);
    const double center = static_cast<double>(odf[i]);
    const double right  = static_cast<double>(odf[i + 1]);

    const double denom = left - 2.0 * center + right;
    if (denom == 0.0) return OdfPeakEstimate{0.0, center};

    const double offset = 0.5 * (left - right) / denom;
    const double value   = center - 0.25 * (left - right) * offset;
    return OdfPeakEstimate{offset, value};
}

}  // namespace

std::vector<PickedPeak> pickPeaks(std::span<const float> odf, double hopSeconds, PeakPickConfig config) {
    std::vector<PickedPeak> out;
    if (odf.size() < 3) return out;

    const std::size_t n = odf.size();
    const std::size_t minInterOnsetFrames =
        hopSeconds > 0.0 ? static_cast<std::size_t>(std::max(1.0, config.minInterOnsetSeconds / hopSeconds)) : 1;

    std::ptrdiff_t lastOnsetFrame = -static_cast<std::ptrdiff_t>(minInterOnsetFrames) - 1;

    for (std::size_t n_ = 1; n_ + 1 < n; ++n_) {
        const std::size_t wLo = n_ >= config.localMaxHalfWindow ? n_ - config.localMaxHalfWindow : 0;
        const std::size_t wHi = std::min(n - 1, n_ + config.localMaxHalfWindow);

        const float local = odf[n_];
        bool isLocalMax = true;
        for (std::size_t k = wLo; k <= wHi; ++k) {
            if (k != n_ && odf[k] > local) { isLocalMax = false; break; }
        }
        if (!isLocalMax) continue;

        const std::size_t mLo = n_ >= config.meanHalfWindow ? n_ - config.meanHalfWindow : 0;
        const std::size_t mHi = std::min(n - 1, n_ + config.meanHalfWindow);
        double sum = 0.0;
        for (std::size_t k = mLo; k <= mHi; ++k) sum += static_cast<double>(odf[k]);
        const double mean = sum / static_cast<double>(mHi - mLo + 1);

        if (static_cast<double>(local) < mean + config.delta) continue;

        const std::ptrdiff_t signedIndex = static_cast<std::ptrdiff_t>(n_);
        if (signedIndex - lastOnsetFrame <= static_cast<std::ptrdiff_t>(minInterOnsetFrames)) continue;

        const auto refined = interpolateOdfPeak(odf, n_);

        PickedPeak peak;
        peak.frameIndex     = n_;
        peak.subFrameOffset = refined.offset;
        peak.strength        = static_cast<float>(refined.value);
        out.push_back(peak);

        lastOnsetFrame = signedIndex;
    }

    return out;
}

}  // namespace aud::beats

#include "freq_mapping.hpp"

#include <algorithm>
#include <cmath>

#include "../fft/scaling.hpp"

namespace aud::spectrogram {

namespace {

double hzToScale(FreqAxis axis, double hz) noexcept {
    switch (axis) {
        case FreqAxis::Linear:
            return hz;
        case FreqAxis::Log:
            return std::log2(std::max(hz, 1e-6));
        case FreqAxis::Mel:
            return 2595.0 * std::log10(1.0 + hz / 700.0);
        case FreqAxis::Bark:
            return 13.0 * std::atan(0.00076 * hz) + 3.5 * std::atan((hz / 7500.0) * (hz / 7500.0));
    }
    return hz;
}

// Closed-form inverse for Linear/Log/Mel; Bark has no simple closed form (a sum of two arctangents)
// so it's inverted by bisection — cheap here since this runs O(outputRows) times per config change,
// never per frame or per tile.
double scaleToHz(FreqAxis axis, double scale, double nyquistHz) noexcept {
    switch (axis) {
        case FreqAxis::Linear:
            return scale;
        case FreqAxis::Log:
            return std::exp2(scale);
        case FreqAxis::Mel:
            return 700.0 * (std::pow(10.0, scale / 2595.0) - 1.0);
        case FreqAxis::Bark: {
            double lo = 0.0;
            double hi = nyquistHz;
            for (int i = 0; i < 40; ++i) {
                const double mid    = 0.5 * (lo + hi);
                const double midVal = hzToScale(FreqAxis::Bark, mid);
                if (midVal < scale) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            return 0.5 * (lo + hi);
        }
    }
    return scale;
}

}  // namespace

Result<FreqMapping> FreqMapping::create(FreqAxis axis, double minHz, double sampleRate,
                                         std::size_t fftBinCount, std::size_t outputRows) {
    if (sampleRate <= 0.0 || fftBinCount < 2 || outputRows == 0) {
        return Error{ErrorCode::InvalidArgument, "spectrogram.freq_mapping", "invalid arguments"};
    }

    const double nyquistHz = sampleRate / 2.0;
    const double loHz      = axis == FreqAxis::Linear ? 0.0 : std::clamp(minHz, 1.0, nyquistHz - 1.0);
    const double sLo       = hzToScale(axis, loHz);
    const double sHi       = hzToScale(axis, nyquistHz);
    const double step      = (sHi - sLo) / static_cast<double>(outputRows);

    // fftSize isn't known directly here (only bin count is), but StftProcessor/FrameComputer both
    // define binFrequencyHz(bin) = bin * sampleRate / fftSize with fftBinCount == fftSize/2+1, so
    // binWidth is recoverable without the caller passing fftSize separately.
    const std::size_t fftSize   = 2 * (fftBinCount - 1);
    const double       binWidth = sampleRate / static_cast<double>(fftSize);

    FreqMapping mapping;
    mapping.m_rows.resize(outputRows);
    for (std::size_t row = 0; row < outputRows; ++row) {
        const double sRowLo  = sLo + step * static_cast<double>(row);
        const double sRowHi  = sLo + step * static_cast<double>(row + 1);
        const double sCenter = 0.5 * (sRowLo + sRowHi);

        const double freqLo    = scaleToHz(axis, sRowLo, nyquistHz);
        const double freqHi    = scaleToHz(axis, sRowHi, nyquistHz);
        const double centerHz  = scaleToHz(axis, sCenter, nyquistHz);

        FreqRow r;
        r.centerHz = centerHz;

        const std::size_t maxBin = fftBinCount - 1;
        std::size_t       binLo  = static_cast<std::size_t>(std::clamp(std::floor(freqLo / binWidth), 0.0, static_cast<double>(maxBin)));
        std::size_t       binHi  = static_cast<std::size_t>(std::clamp(std::ceil(freqHi / binWidth), 0.0, static_cast<double>(fftBinCount)));

        if (binHi <= binLo + 1) {
            // Row narrower than a bin: interpolate between the two bins straddling the centre.
            const double centerBin = std::clamp(centerHz / binWidth, 0.0, static_cast<double>(maxBin));
            std::size_t  lo        = static_cast<std::size_t>(std::floor(centerBin));
            if (lo >= maxBin) {
                lo = maxBin - 1;
            }
            r.interpolate = true;
            r.binLo       = static_cast<std::uint32_t>(lo);
            r.binHi       = static_cast<std::uint32_t>(lo + 1);
            r.frac        = static_cast<float>(centerBin - static_cast<double>(lo));
        } else {
            r.interpolate = false;
            r.binLo       = static_cast<std::uint32_t>(binLo);
            r.binHi       = static_cast<std::uint32_t>(std::min(binHi, fftBinCount));
        }

        mapping.m_rows[row] = r;
    }

    return mapping;
}

void FreqMapping::mapToDb(std::span<const float> magnitudes, std::span<double> outDb) const noexcept {
    for (std::size_t row = 0; row < m_rows.size() && row < outDb.size(); ++row) {
        const FreqRow& r = m_rows[row];
        double         amplitude;
        if (r.interpolate) {
            const double a = magnitudes[r.binLo];
            const double b = magnitudes[r.binHi];
            amplitude      = a + (b - a) * r.frac;
        } else {
            double maxAmp = 0.0;
            for (std::uint32_t bin = r.binLo; bin < r.binHi; ++bin) {
                maxAmp = std::max(maxAmp, static_cast<double>(magnitudes[bin]));
            }
            amplitude = maxAmp;
        }
        outDb[row] = aud::fft::toDb(amplitude);
    }
}

std::size_t FreqMapping::nearestRow(double hz) const noexcept {
    std::size_t best      = 0;
    double      bestDelta = std::abs(m_rows.empty() ? 0.0 : m_rows[0].centerHz - hz);
    for (std::size_t row = 1; row < m_rows.size(); ++row) {
        const double delta = std::abs(m_rows[row].centerHz - hz);
        if (delta < bestDelta) {
            bestDelta = delta;
            best       = row;
        }
    }
    return best;
}

}  // namespace aud::spectrogram

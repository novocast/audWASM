#include "odf.hpp"

#include <algorithm>
#include <cmath>

namespace aud::beats {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Wrap a phase difference into (-pi, pi].
double wrapPhase(double phase) noexcept {
    while (phase > kPi) phase -= 2.0 * kPi;
    while (phase <= -kPi) phase += 2.0 * kPi;
    return phase;
}

std::size_t binForFrequency(double hz, SampleRate sampleRate, std::size_t fftSize) noexcept {
    if (sampleRate == 0) return 0;
    const double bin = hz * static_cast<double>(fftSize) / static_cast<double>(sampleRate);
    return bin < 0.0 ? 0 : static_cast<std::size_t>(bin);
}
}  // namespace

OdfComputer::OdfComputer(std::size_t binCount, SampleRate sampleRate, OdfConfig config)
    : m_config(config), m_binCount(binCount), m_sampleRate(sampleRate) {
    // fftSize isn't known directly here (only bin count = fftSize/2+1), reconstruct it for the
    // band-boundary bin lookup.
    const std::size_t fftSize = (binCount - 1) * 2;
    m_lowBandBin  = binForFrequency(config.lowBandHz, sampleRate, fftSize);
    m_highBandBin = binForFrequency(config.highBandHz, sampleRate, fftSize);

    m_prevLogMag.assign(binCount, 0.0f);
    m_prevPhase.assign(binCount, 0.0f);
    m_prevPrevPhase.assign(binCount, 0.0f);
}

OdfSample OdfComputer::push(std::span<const float> magnitudes, std::span<const std::complex<float>> complexBins) {
    OdfSample out;
    if (magnitudes.size() != m_binCount || complexBins.size() != m_binCount) {
        return out;  // malformed input — caller error, report nothing rather than reading OOB
    }

    double flux = 0.0, lowFlux = 0.0, midFlux = 0.0, highFlux = 0.0;
    double complexDeviation = 0.0;
    double hfc = 0.0;

    const double lambda = m_config.logCompressionLambda;

    for (std::size_t k = 0; k < m_binCount; ++k) {
        const double mag    = static_cast<double>(magnitudes[k]);
        const double logMag = std::log1p(lambda * mag);

        double bandContribution = 0.0;
        if (m_hasPrev) {
            bandContribution = std::max(0.0, logMag - static_cast<double>(m_prevLogMag[k]));
            flux += bandContribution;
            if (k < m_lowBandBin) {
                lowFlux += bandContribution;
            } else if (k < m_highBandBin) {
                midFlux += bandContribution;
            } else {
                highFlux += bandContribution;
            }
        }

        hfc += static_cast<double>(k) * mag * mag;

        const double phase = std::arg(complexBins[k]);
        if (m_hasPrevPrev) {
            const double predictedPhase = wrapPhase(2.0 * static_cast<double>(m_prevPhase[k]) -
                                                      static_cast<double>(m_prevPrevPhase[k]));
            // Predicted magnitude: no strong prior beyond "similar to last frame" — recover the
            // previous frame's linear magnitude from its retained log-compressed value and use
            // that as the predicted magnitude (standard complex-domain ODF: predicted magnitude ==
            // previous observed magnitude, predicted phase == linear extrapolation of the last two
            // phases).
            const double prevMagLinear = (std::exp(static_cast<double>(m_prevLogMag[k])) - 1.0) / std::max(lambda, 1e-9);
            const std::complex<double> predicted = std::polar(std::max(prevMagLinear, 0.0), predictedPhase);
            const std::complex<double> observed(static_cast<double>(complexBins[k].real()),
                                                  static_cast<double>(complexBins[k].imag()));
            complexDeviation += std::abs(observed - predicted);
        }

        m_prevPrevPhase[k] = m_prevPhase[k];
        m_prevPhase[k]     = static_cast<float>(phase);
        m_prevLogMag[k]    = static_cast<float>(logMag);
    }

    out.flux          = static_cast<float>(flux);
    out.complexDomain = static_cast<float>(complexDeviation);
    out.hfc           = static_cast<float>(hfc);
    out.lowBandFlux    = static_cast<float>(lowFlux);
    out.midBandFlux    = static_cast<float>(midFlux);
    out.highBandFlux   = static_cast<float>(highFlux);

    const double bandTotal = lowFlux + midFlux + highFlux;
    if (bandTotal > 0.0) {
        constexpr double kBandShareThreshold = 0.2;  // a band "contributes" if it carries >=20% of the flux
        if (lowFlux / bandTotal >= kBandShareThreshold) out.bandMask |= 0x1;
        if (midFlux / bandTotal >= kBandShareThreshold) out.bandMask |= 0x2;
        if (highFlux / bandTotal >= kBandShareThreshold) out.bandMask |= 0x4;
    }

    m_hasPrevPrev = m_hasPrev;
    m_hasPrev     = true;

    return out;
}

}  // namespace aud::beats

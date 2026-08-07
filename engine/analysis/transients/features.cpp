#include "features.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "../../fft/real_fft.hpp"
#include "../../fft/windows.hpp"

namespace aud::transients {

namespace {

std::size_t nextSupportedFftSize(std::size_t n) {
    std::size_t size = 2;
    while (size < n || !aud::fft::isSupportedFftSize(size)) size <<= 1;
    return size;
}

}  // namespace

SpectralFeatures computeSpectralFeatures(std::span<const Sample> mono, SampleRate sampleRate, FeatureConfig config) {
    SpectralFeatures out;
    if (mono.empty() || sampleRate == 0) return out;

    const std::size_t wantedSamples =
        std::max<std::size_t>(2, static_cast<std::size_t>(config.windowMs * 0.001 * static_cast<double>(sampleRate)));
    const std::size_t windowLength = std::min(mono.size(), wantedSamples);
    const std::size_t fftSize      = nextSupportedFftSize(windowLength);

    auto fftResult = aud::fft::RealFft::create(fftSize);
    if (!fftResult.has_value()) return out;
    auto fft = std::move(fftResult).value();

    std::vector<float> window(windowLength);
    aud::fft::generateWindow(aud::fft::WindowType::Hann, /*periodic=*/true, /*kaiserBeta=*/8.6, window);

    std::vector<float> input(fftSize, 0.0f);
    for (std::size_t i = 0; i < windowLength; ++i) input[i] = mono[i] * window[i];

    std::vector<std::complex<float>> bins(fftSize / 2 + 1);
    fft->forward(input, bins);

    const std::size_t binCount = bins.size();
    std::vector<float> magnitude(binCount);
    for (std::size_t i = 0; i < binCount; ++i) magnitude[i] = std::abs(bins[i]);

    const double binHz = static_cast<double>(sampleRate) / static_cast<double>(fftSize);

    // --- Centroid / spread ---
    double magSum = 0.0;
    double weightedSum = 0.0;
    for (std::size_t i = 0; i < binCount; ++i) {
        magSum += magnitude[i];
        weightedSum += magnitude[i] * (static_cast<double>(i) * binHz);
    }
    const double centroid = magSum > 0.0 ? weightedSum / magSum : 0.0;
    out.spectralCentroidHz = static_cast<float>(centroid);

    double spreadAccum = 0.0;
    for (std::size_t i = 0; i < binCount; ++i) {
        const double freq = static_cast<double>(i) * binHz;
        spreadAccum += magnitude[i] * (freq - centroid) * (freq - centroid);
    }
    out.spectralSpreadHz = static_cast<float>(magSum > 0.0 ? std::sqrt(spreadAccum / magSum) : 0.0);

    // --- Rolloff (85% / 95% of cumulative power) ---
    double totalPower = 0.0;
    for (std::size_t i = 0; i < binCount; ++i) totalPower += static_cast<double>(magnitude[i]) * magnitude[i];

    auto rolloffAt = [&](double fraction) -> float {
        if (totalPower <= 0.0) return 0.0f;
        const double target = totalPower * fraction;
        double       cumulative = 0.0;
        for (std::size_t i = 0; i < binCount; ++i) {
            cumulative += static_cast<double>(magnitude[i]) * magnitude[i];
            if (cumulative >= target) return static_cast<float>(static_cast<double>(i) * binHz);
        }
        return static_cast<float>(static_cast<double>(binCount - 1) * binHz);
    };
    out.rolloff85Hz = rolloffAt(config.rolloffLowFrac);
    out.rolloff95Hz = rolloffAt(config.rolloffHighFrac);

    // --- Flatness: geometric mean / arithmetic mean of the power spectrum (excludes DC, which is
    // dominated by any residual offset rather than the transient's spectral shape). ---
    if (binCount > 1) {
        constexpr double kEps  = 1e-12;
        double            logSum = 0.0;
        double            sum    = 0.0;
        std::size_t        count  = 0;
        for (std::size_t i = 1; i < binCount; ++i) {
            const double power = static_cast<double>(magnitude[i]) * magnitude[i] + kEps;
            logSum += std::log(power);
            sum += power;
            ++count;
        }
        if (count > 0 && sum > 0.0) {
            const double geoMean = std::exp(logSum / static_cast<double>(count));
            const double arithMean = sum / static_cast<double>(count);
            out.spectralFlatness = static_cast<float>(std::clamp(geoMean / arithMean, 0.0, 1.0));
        }
    }

    // --- Band energy ratios ---
    double bandEnergy[4] = {0.0, 0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < binCount; ++i) {
        const double freq  = static_cast<double>(i) * binHz;
        const double power = static_cast<double>(magnitude[i]) * magnitude[i];
        if (freq < config.lowBandHz) {
            bandEnergy[0] += power;
        } else if (freq < config.lowMidBandHz) {
            bandEnergy[1] += power;
        } else if (freq < config.midBandHz) {
            bandEnergy[2] += power;
        } else {
            bandEnergy[3] += power;
        }
    }
    if (totalPower > 0.0) {
        for (int b = 0; b < 4; ++b) out.bandEnergyRatio[b] = static_cast<float>(bandEnergy[b] / totalPower);
    }

    return out;
}

}  // namespace aud::transients

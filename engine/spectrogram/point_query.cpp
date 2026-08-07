#include "point_query.hpp"

#include <algorithm>
#include <cmath>

#include "../fft/peak_interp.hpp"
#include "frame_computer.hpp"

namespace aud::spectrogram {

Result<PointResult> queryPoint(const AudioBuffer& buffer, ChannelIndex channel, double timeSeconds,
                                double targetHz, const TileConfig& config) {
    if (channel >= buffer.channelCount()) {
        return Error{ErrorCode::InvalidArgument, "spectrogram.point_query", "channel out of range"};
    }

    AUD_TRY_ASSIGN(computer,
                    FrameComputer::create(config.fftSize, static_cast<aud::fft::WindowType>(config.window),
                                           static_cast<aud::fft::SpectrumScaling>(config.scaling),
                                           buffer.sampleRate()));

    const auto centerFrame = static_cast<FrameIndex>(timeSeconds * static_cast<double>(buffer.sampleRate()) + 0.5);
    const auto magnitudes  = computer.computeFrame(buffer, channel, centerFrame);

    const std::size_t binCount = computer.binCount();
    if (binCount < 3) {
        return Error{ErrorCode::InvalidArgument, "spectrogram.point_query", "fftSize too small"};
    }

    const double      binWidth = static_cast<double>(buffer.sampleRate()) / static_cast<double>(config.fftSize);
    const std::size_t lastInterpolable = binCount - 2;  // interpolatePeak needs bin-1 and bin+1
    std::size_t       target = static_cast<std::size_t>(
        std::clamp(std::llround(targetHz / binWidth), static_cast<long long>(1), static_cast<long long>(lastInterpolable)));

    // Search a small neighbourhood for the true local maximum nearest the target bin; fall back to
    // the neighbourhood's argmax if nothing is a strict local max (e.g. a flat/noisy region).
    const std::size_t searchLo = target > 4 ? target - 4 : 1;
    const std::size_t searchHi = std::min(target + 4, lastInterpolable);

    std::size_t bestStrictPeak = 0;
    bool        foundStrictPeak = false;
    std::size_t bestArgmax      = target;
    float       bestArgmaxValue = magnitudes[target];

    for (std::size_t i = searchLo; i <= searchHi; ++i) {
        if (magnitudes[i] > bestArgmaxValue) {
            bestArgmaxValue = magnitudes[i];
            bestArgmax      = i;
        }
        const bool isStrictPeak = magnitudes[i] > magnitudes[i - 1] && magnitudes[i] > magnitudes[i + 1];
        if (isStrictPeak &&
            (!foundStrictPeak ||
             std::llabs(static_cast<long long>(i) - static_cast<long long>(target)) <
                 std::llabs(static_cast<long long>(bestStrictPeak) - static_cast<long long>(target)))) {
            bestStrictPeak  = i;
            foundStrictPeak = true;
        }
    }

    const std::size_t peakBin = foundStrictPeak ? bestStrictPeak : bestArgmax;
    const aud::fft::PeakEstimate peak = aud::fft::interpolatePeak(magnitudes, peakBin);

    PointResult result;
    result.frequencyHz = (static_cast<double>(peakBin) + peak.binOffset) * binWidth;
    result.magnitudeDb = aud::fft::toDb(std::exp(peak.logMagnitude));
    return result;
}

}  // namespace aud::spectrogram

#include "dropout_detector.hpp"

#include <cmath>

namespace aud::transients {

std::vector<DropoutRun> detectDropouts(std::span<const Sample> mono, SampleRate sampleRate,
                                        DropoutDetectorConfig config) {
    std::vector<DropoutRun> out;
    if (mono.empty() || sampleRate == 0) return out;

    const float thresholdLinear = static_cast<float>(std::pow(10.0, config.thresholdDbfs / 20.0));
    const std::size_t minSamples =
        static_cast<std::size_t>(config.minDurationMs * 0.001 * static_cast<double>(sampleRate));
    const std::size_t maxSamples =
        static_cast<std::size_t>(config.maxDurationMs * 0.001 * static_cast<double>(sampleRate));

    const std::size_t total = mono.size();
    std::size_t       i     = 0;
    while (i < total) {
        if (std::fabs(mono[i]) > thresholdLinear) {
            ++i;
            continue;
        }
        const std::size_t runBegin = i;
        while (i < total && std::fabs(mono[i]) <= thresholdLinear) ++i;
        const std::size_t runLength = i - runBegin;
        if (runLength >= minSamples && runLength <= maxSamples) {
            out.push_back(DropoutRun{static_cast<FrameIndex>(runBegin), static_cast<FrameIndex>(i)});
        }
    }

    return out;
}

}  // namespace aud::transients

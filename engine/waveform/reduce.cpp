#include "reduce.hpp"

#include <algorithm>
#include <cmath>

#include "../util/accumulate.hpp"

namespace aud::waveform {

WaveformBin reduceOneBin(std::span<const Sample> samples) noexcept {
    WaveformBin bin;
    if (samples.empty()) {
        return bin;  // zeroed — matches both "no data yet" and the digital-silence contract
    }

    Sample lo = samples[0];
    Sample hi = samples[0];
    for (Sample s : samples) {
        lo = std::min(lo, s);
        hi = std::max(hi, s);
    }

    const double sumSq = pairwiseSumSquares<double>(samples);
    const double rms   = std::sqrt(sumSq / static_cast<double>(samples.size()));

    bin.min     = lo;
    bin.max     = hi;
    bin.rms     = static_cast<Sample>(rms);
    bin.absPeak = std::max(-lo, hi);
    return bin;
}

void reduceToBins(std::span<const Sample> samples, std::size_t binFrames, std::vector<WaveformBin>& out) {
    if (binFrames == 0) {
        return;
    }
    for (std::size_t offset = 0; offset < samples.size(); offset += binFrames) {
        const std::size_t count = std::min(binFrames, samples.size() - offset);
        out.push_back(reduceOneBin(samples.subspan(offset, count)));
    }
}

}  // namespace aud::waveform

#include "refine_timing.hpp"

#include <algorithm>
#include <cmath>
#include <deque>

namespace aud::transients {

namespace {

std::size_t msToSamples(double ms, SampleRate sampleRate) {
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(ms * 0.001 * static_cast<double>(sampleRate))));
}

float energyOver(std::span<const Sample> mono, std::size_t begin, std::size_t end) {
    float sum = 0.0f;
    for (std::size_t i = begin; i < end; ++i) sum += mono[i] * mono[i];
    return sum;
}

}  // namespace

RefinedTiming refineTransientTiming(std::span<const Sample> mono, SampleRate sampleRate, FrameIndex approxFrame,
                                     RefineTimingConfig config) {
    RefinedTiming out;
    if (mono.empty() || sampleRate == 0 || approxFrame < 0 || static_cast<std::size_t>(approxFrame) >= mono.size()) {
        return out;
    }

    const std::size_t total            = mono.size();
    const std::size_t searchHalf       = msToSamples(config.searchWindowMs, sampleRate);
    const std::size_t attackWindow     = msToSamples(config.attackWindowMs, sampleRate);
    const std::size_t precedingWindow  = msToSamples(config.precedingWindowMs, sampleRate);
    const std::size_t approx           = static_cast<std::size_t>(approxFrame);

    const std::size_t lo = approx > searchHalf ? approx - searchHalf : 0;
    const std::size_t hi = std::min(total, approx + searchHalf);

    // --- Steepest short-term energy rise: attackEnergy (ahead) vs precedingEnergy (behind), at
    // every candidate point inside the search window (doc: "1ms attack window vs 5ms preceding
    // window"). ---
    const std::size_t testLo = lo + precedingWindow < hi ? lo + precedingWindow : lo;
    const std::size_t testHi = hi > attackWindow ? hi - attackWindow : hi;

    std::size_t bestFrame = approx;
    float       bestRatio = -1.0f;
    constexpr float kEps   = 1e-12f;
    for (std::size_t i = testLo; i < testHi; ++i) {
        const float attackEnergy    = energyOver(mono, i, i + attackWindow);
        const float precedingEnergy = energyOver(mono, i - precedingWindow, i);
        const float ratio           = attackEnergy / (precedingEnergy + kEps);
        // >= (not >): a genuine transient can hold its maximum ratio over a short plateau (the
        // clearest case is an idealised single-sample impulse, where every test point whose attack
        // window still contains it ties) — the *latest* such point is closest to the actual event,
        // which is what the doc's +-1ms accuracy criterion needs.
        if (ratio >= bestRatio) {
            bestRatio  = ratio;
            bestFrame  = i;
        }
    }
    out.attackFrame = static_cast<FrameIndex>(bestFrame);

    // --- Walk back to the last zero crossing before the attack (doc: "what a human would call the
    // start of the transient"). ---
    std::size_t startFrame = lo;
    if (bestFrame > lo) {
        for (std::size_t i = bestFrame; i > lo; --i) {
            const bool signChange = (mono[i - 1] >= 0.0f) != (mono[i] >= 0.0f);
            if (signChange) {
                startFrame = i;
                break;
            }
        }
    }
    out.startFrame = static_cast<FrameIndex>(startFrame);

    // --- Local peak following the attack. ---
    const std::size_t peakSearchSamples = msToSamples(config.peakSearchMs, sampleRate);
    const std::size_t peakHi            = std::min(total, bestFrame + peakSearchSamples);
    std::size_t        peakFrame        = bestFrame;
    float               peakAmplitude   = std::fabs(mono[bestFrame]);
    for (std::size_t i = bestFrame; i < peakHi; ++i) {
        const float a = std::fabs(mono[i]);
        if (a > peakAmplitude) {
            peakAmplitude = a;
            peakFrame     = i;
        }
    }
    out.peakAmplitude = peakAmplitude;

    // --- Attack time: 10% -> 90% of the local peak, walking forward from startFrame. ---
    if (peakAmplitude > 0.0f) {
        const float lowThreshold  = peakAmplitude * static_cast<float>(config.attackLowFraction);
        const float highThreshold = peakAmplitude * static_cast<float>(config.attackHighFraction);

        std::size_t lowFrame  = startFrame;
        std::size_t highFrame = peakFrame;
        bool        foundLow  = false;
        bool        foundHigh = false;
        for (std::size_t i = startFrame; i <= peakFrame; ++i) {
            const float a = std::fabs(mono[i]);
            if (!foundLow && a >= lowThreshold) {
                lowFrame = i;
                foundLow = true;
            }
            if (!foundHigh && a >= highThreshold) {
                highFrame = i;
                foundHigh = true;
                break;
            }
        }
        if (highFrame >= lowFrame) {
            out.attackTimeMs =
                static_cast<float>((highFrame - lowFrame) * 1000.0 / static_cast<double>(sampleRate));
        }

        // --- Decay time: peak -> decayThresholdDb below the peak. Measured against a trailing
        // sliding-window envelope (max |x| over the last `envelopeWindowMs`), not the instantaneous
        // sample: an oscillating signal (a decaying tone, a snare's resonant body) crosses zero
        // every half-cycle long before its *envelope* has actually decayed, and testing the raw
        // sample would report a wildly premature decay time. The window needs to span at least one
        // full cycle of the lowest frequency content expected (config default covers down to ~65Hz).
        const float decayThresholdLinear =
            peakAmplitude * static_cast<float>(std::pow(10.0, config.decayThresholdDb / 20.0));
        const std::size_t decaySearchSamples = msToSamples(config.maxDecaySearchMs, sampleRate);
        const std::size_t decayHi            = std::min(total, peakFrame + decaySearchSamples);
        const std::size_t envWindow          = msToSamples(config.envelopeWindowMs, sampleRate);

        std::deque<std::size_t> maxDeque;  // indices into mono, decreasing |mono[.]|, within the trailing window
        for (std::size_t i = peakFrame; i < decayHi; ++i) {
            const float a = std::fabs(mono[i]);
            while (!maxDeque.empty() && std::fabs(mono[maxDeque.back()]) <= a) maxDeque.pop_back();
            maxDeque.push_back(i);
            while (maxDeque.front() + envWindow <= i) maxDeque.pop_front();

            if (i >= peakFrame + envWindow - 1) {
                const float envelope = std::fabs(mono[maxDeque.front()]);
                if (envelope <= decayThresholdLinear) {
                    const std::size_t decayFrame = i - envWindow + 1;
                    out.decayTimeMs =
                        static_cast<float>((decayFrame - peakFrame) * 1000.0 / static_cast<double>(sampleRate));
                    break;
                }
            }
        }
    }

    return out;
}

}  // namespace aud::transients

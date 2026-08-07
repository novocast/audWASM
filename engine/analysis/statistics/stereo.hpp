#pragma once

// Stereo relationship statistics (M09 "Stereo correlation"): global Pearson correlation, a 50ms
// correlation-over-time series (so the UI can localise a phase problem, not just know one exists
// somewhere), balance, and mono-compatibility loss in dB.

#include <cstdint>
#include <span>
#include <vector>

#include "../../util/audio_types.hpp"

namespace aud::statistics {

struct StereoStatistics {
    double correlation         = 0.0;  // Pearson r, DC removed, over the whole file
    double balanceDb           = 0.0;  // 20*log10(rmsR / rmsL)
    double monoCompatibilityDb = 0.0;  // energy((L+R)/2) vs mean(energy(L), energy(R)), in dB; 0 = no loss

    std::vector<float> correlationSeries;  // 50ms resolution, one value per window
};

// Streaming accumulator for the cross-channel terms correlation needs (Sigma L*R, and the L/R sums
// ChannelAccumulator already tracks per-channel aren't enough on their own). Single pass, fed
// alongside the two ChannelAccumulators for L and R.
class StereoAccumulator {
public:
    void begin(SampleRate sampleRate) noexcept;

    // `left`/`right` must be the same length (one chunk's worth of frames for the two channels).
    void process(std::span<const Sample> left, std::span<const Sample> right) noexcept;

    void finish() noexcept;

    // `meanL`/`meanR` are the exact per-channel means from each side's ChannelAccumulator, `varL`/
    // `varR` its population variance (both already DC-correct) — correlation needs all four plus
    // this accumulator's own Sigma L*R and n.
    [[nodiscard]] StereoStatistics computeStatistics(double meanL, double varL, double sumSqL, double meanR,
                                                       double varR, double sumSqR) const;

private:
    std::uint64_t m_n            = 0;
    double        m_sumLR        = 0.0;

    std::uint64_t m_windowFrames = 0;
    std::uint64_t m_windowCount  = 0;
    double        m_windowSumL   = 0.0;
    double        m_windowSumR   = 0.0;
    double        m_windowSumLL  = 0.0;
    double        m_windowSumRR  = 0.0;
    double        m_windowSumLR  = 0.0;

    std::vector<float> m_correlationSeries;
};

}  // namespace aud::statistics

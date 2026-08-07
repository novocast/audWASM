#pragma once

// M14's per-transient spectral features — see documentation/tasks/M14-transient-detection.md
// "Classification". Computed once, over a short (~30ms) window starting at the refined attack
// frame, and consumed both by classifier.hpp (rule-based v1) and reported directly to the UI/JSON
// (the doc's explicit design for an ML classifier to drop in later on the same features).
//
// A one-shot windowed FFT, not M06's StftProcessor — that class is built for a long in-order
// stream of overlapping frames; a transient feature window is a single, isolated analysis over
// PCM the caller already has random access to (see transient_analyzer.hpp's header comment).

#include <array>
#include <cstddef>
#include <span>

#include "../../util/audio_types.hpp"

namespace aud::transients {

struct FeatureConfig {
    double windowMs        = 30.0;
    double rolloffLowFrac  = 0.85;
    double rolloffHighFrac = 0.95;

    // Band edges, Hz (doc: "low (<150Hz) / low-mid / mid / high (>5kHz)").
    double lowBandHz    = 150.0;
    double lowMidBandHz = 1000.0;
    double midBandHz    = 5000.0;
};

struct SpectralFeatures {
    float spectralCentroidHz = 0.0f;
    float spectralSpreadHz   = 0.0f;
    float rolloff85Hz        = 0.0f;
    float rolloff95Hz        = 0.0f;
    float spectralFlatness   = 0.0f;  // 0 (tonal) .. 1 (noisy/flat)

    // [low, lowMid, mid, high], each a fraction of total band energy, summing to ~1.
    std::array<float, 4> bandEnergyRatio{};
};

// `mono` should start at (or very near) the transient's refined attack frame; at least a few
// samples are required or a zeroed SpectralFeatures is returned. Internally zero-pads to the next
// FFT-supported size, so any window length is accepted.
[[nodiscard]] SpectralFeatures computeSpectralFeatures(std::span<const Sample> mono, SampleRate sampleRate,
                                                         FeatureConfig config = {});

}  // namespace aud::transients

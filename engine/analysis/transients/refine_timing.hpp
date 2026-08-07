#pragma once

// M14's sample-accurate timing refinement — see documentation/tasks/M14-transient-detection.md
// "Sample-accurate timing". M13's onsets are located to within an STFT hop (~11.6ms at the default
// config), which is visibly wrong for a transient overlay drawn on a sample-zoomed waveform.
//
// Algorithm (doc): around each candidate onset, take a +-20ms window of raw PCM and find the point
// of maximum short-term energy rise using a small sliding-window energy ratio (1ms attack window vs
// 5ms preceding window). The steepest-rise point is `attackFrame`; walking back from it to the last
// zero crossing gives `startFrame` — what a human would call the start of the transient and what an
// editor would cut on. Attack time (10%->90% of the local peak) and decay time are measured from the
// same window and feed both the classifier (features.hpp) and the UI directly.
//
// Operates on a single mono PCM series with random access (the whole track, retained by
// TransientAnalyzer — see transient_analyzer.hpp's header comment for why that differs from M13's
// pure-streaming shape).

#include <cstddef>
#include <span>

#include "../../util/audio_types.hpp"

namespace aud::transients {

struct RefineTimingConfig {
    double searchWindowMs     = 20.0;  // +- window around the candidate frame
    double attackWindowMs     = 1.0;   // short-term energy window, ahead of the test point
    double precedingWindowMs  = 5.0;   // short-term energy window, behind the test point
    double attackLowFraction  = 0.10;  // 10% of local peak
    double attackHighFraction = 0.90;  // 90% of local peak
    double peakSearchMs       = 30.0;  // how far past attackFrame to look for the local peak
    double decayThresholdDb   = -20.0; // decay time = time to fall this far below the peak
    double maxDecaySearchMs   = 500.0;
    double envelopeWindowMs   = 20.0;  // trailing window for the decay envelope — see refine_timing.cpp
};

struct RefinedTiming {
    FrameIndex startFrame  = kNoFrame;  // last zero crossing before attackFrame
    FrameIndex attackFrame = kNoFrame;  // steepest short-term energy rise
    float      peakAmplitude = 0.0f;    // |sample| at the local peak following attackFrame
    float      attackTimeMs  = 0.0f;    // 10%->90% of peakAmplitude
    float      decayTimeMs   = 0.0f;    // peak -> decayThresholdDb below peak (0 if it never decays within the window)
};

// `mono` is the whole track's mono-mixdown PCM; `approxFrame` is M13's onset frame (or any other
// coarse candidate). Returns kNoFrame fields if `mono` is empty or `approxFrame` is out of range —
// callers should skip the candidate rather than fabricate a value.
[[nodiscard]] RefinedTiming refineTransientTiming(std::span<const Sample> mono, SampleRate sampleRate,
                                                    FrameIndex approxFrame, RefineTimingConfig config = {});

}  // namespace aud::transients

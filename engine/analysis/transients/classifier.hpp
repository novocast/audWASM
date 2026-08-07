#pragma once

// M14's rule-based transient classifier — see documentation/tasks/M14-transient-detection.md
// "Classification" and its "Decision — a rule-based classifier in v1, not machine learning."
// Explainable (the UI can say *why* it called something a kick), needs no training data, and is
// fast. Every rule here is a named, tunable constant in ClassifierConfig rather than scattered
// magic numbers — the doc's risk-table mitigation for "class boundaries argued about forever": tune
// the table, don't rewrite the logic.
//
// Click/Dropout are never produced here — those come from click_detector.hpp/dropout_detector.hpp,
// which run their own (non-spectral) detectors and assign the classification directly. This
// classifier only ever returns Kick/Snare/HiHat/Percussion/TonalOnset/Unclassified.
//
// Feature design is deliberately reusable by an ML classifier later (the doc's M21/M20 path): the
// rules below are just one function of the same ClassificationInput an ML model would consume.

#include <cstdint>

#include "features.hpp"

namespace aud::transients {

enum class TransientClass : std::uint8_t {
    Kick,
    Snare,
    HiHat,
    Percussion,
    TonalOnset,
    Click,
    Dropout,
    Unclassified,
};

inline constexpr std::size_t kTransientClassCount = 8;

struct ClassifierConfig {
    // Soft-margin widths used by every rangeScore()/aboveScore()/belowScore() call below — see
    // classifier.cpp. A candidate scores 1.0 fully inside the ideal band/threshold and ramps to 0.0
    // over the given width outside it, rather than hard cutoffs that would make the boundary table
    // brittle to the very tuning the doc's risk table anticipates.
    float minConfidence = 0.5f;  // below this, report Unclassified rather than force a bucket

    float kickLowBandThreshold  = 0.60f;
    float kickDecayLowMs        = 50.0f;
    float kickDecayHighMs       = 300.0f;
    float kickCentroidMaxHz     = 300.0f;

    float snareBodyMin          = 0.15f;
    float snareFlatnessMin      = 0.40f;
    float snareDecayLowMs       = 100.0f;
    float snareDecayHighMs      = 400.0f;

    float hihatHighBandThreshold = 0.70f;
    float hihatFlatnessMin       = 0.50f;

    float tonalFlatnessMax      = 0.30f;
    float tonalAttackLowMs      = 1.0f;
    float tonalAttackHighMs     = 30.0f;

    float percussionBodyMin     = 0.30f;
    float percussionDecayLowMs  = 50.0f;
    float percussionDecayHighMs = 400.0f;
    float percussionCentroidMaxHz = 3000.0f;
};

struct ClassificationInput {
    SpectralFeatures features;
    float attackTimeMs   = 0.0f;
    float decayTimeMs    = 0.0f;
    float peakAmplitude  = 0.0f;  // linear, for callers that want it alongside the classification
};

struct Classification {
    TransientClass klass      = TransientClass::Unclassified;
    float          confidence = 0.0f;  // 0..1
};

[[nodiscard]] Classification classifyTransient(const ClassificationInput& input, ClassifierConfig config = {});

}  // namespace aud::transients

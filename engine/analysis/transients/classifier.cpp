#include "classifier.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace aud::transients {

namespace {

// 1.0 inside [lo, hi], ramping to 0.0 over `softness` outside either edge.
float rangeScore(float value, float lo, float hi, float softness) {
    if (value >= lo && value <= hi) return 1.0f;
    const float distance = value < lo ? (lo - value) : (value - hi);
    if (softness <= 0.0f) return 0.0f;
    return std::clamp(1.0f - distance / softness, 0.0f, 1.0f);
}

float aboveScore(float value, float threshold, float softness) {
    if (value >= threshold) return 1.0f;
    if (softness <= 0.0f) return 0.0f;
    return std::clamp(1.0f - (threshold - value) / softness, 0.0f, 1.0f);
}

float belowScore(float value, float threshold, float softness) {
    if (value <= threshold) return 1.0f;
    if (softness <= 0.0f) return 0.0f;
    return std::clamp(1.0f - (value - threshold) / softness, 0.0f, 1.0f);
}

float average(std::initializer_list<float> values) {
    float sum = 0.0f;
    for (float v : values) sum += v;
    return values.size() > 0 ? sum / static_cast<float>(values.size()) : 0.0f;
}

}  // namespace

Classification classifyTransient(const ClassificationInput& input, ClassifierConfig config) {
    const SpectralFeatures& f = input.features;

    // Kick: mostly sub-150Hz energy, low centroid, a few hundred ms of decay.
    float kickScore = 0.0f;
    if (f.bandEnergyRatio[0] >= config.kickLowBandThreshold * 0.6f) {
        kickScore = average({
            aboveScore(f.bandEnergyRatio[0], config.kickLowBandThreshold, 0.2f),
            rangeScore(input.decayTimeMs, config.kickDecayLowMs, config.kickDecayHighMs, 100.0f),
            belowScore(f.spectralCentroidHz, config.kickCentroidMaxHz, 300.0f),
        });
    }

    // Snare: broadband with real body in the 150Hz-5kHz range (our band split doesn't isolate the
    // doc's exact ">2kHz noise" boundary — flatness stands in for "has noisy high content" instead)
    // and a high spectral flatness (noisy, not tonal).
    float snareScore = 0.0f;
    const float snareBody = f.bandEnergyRatio[1] + f.bandEnergyRatio[2];
    if (f.bandEnergyRatio[0] < config.kickLowBandThreshold) {
        snareScore = average({
            aboveScore(snareBody, config.snareBodyMin, 0.15f),
            aboveScore(f.spectralFlatness, config.snareFlatnessMin, 0.2f),
            rangeScore(input.decayTimeMs, config.snareDecayLowMs, config.snareDecayHighMs, 150.0f),
        });
    }

    // Hi-hat / cymbal: dominated by energy above 5kHz, high flatness; decay can be short (closed) or
    // long (open), so decay isn't part of the score.
    float hihatScore = 0.0f;
    if (f.bandEnergyRatio[3] >= config.hihatHighBandThreshold * 0.5f) {
        hihatScore = average({
            aboveScore(f.bandEnergyRatio[3], config.hihatHighBandThreshold, 0.2f),
            aboveScore(f.spectralFlatness, config.hihatFlatnessMin, 0.2f),
        });
    }

    // Tonal onset: harmonic (low flatness), moderate attack — a plucked/struck pitched note.
    float tonalScore = 0.0f;
    if (f.spectralFlatness <= config.tonalFlatnessMax * 2.0f) {
        tonalScore = average({
            belowScore(f.spectralFlatness, config.tonalFlatnessMax, 0.2f),
            rangeScore(input.attackTimeMs, config.tonalAttackLowMs, config.tonalAttackHighMs, 20.0f),
        });
    }

    // Percussion: the catch-all broadband, mid-centroid, moderate-decay bucket for toms/congas/etc
    // that don't match kick/snare/hi-hat's sharper profiles.
    float percussionScore = 0.0f;
    {
        const float body = f.bandEnergyRatio[1] + f.bandEnergyRatio[2];
        percussionScore = average({
            aboveScore(body, config.percussionBodyMin, 0.2f),
            rangeScore(input.decayTimeMs, config.percussionDecayLowMs, config.percussionDecayHighMs, 150.0f),
            belowScore(f.spectralCentroidHz, config.percussionCentroidMaxHz, 1000.0f),
        });
    }

    const std::array<std::pair<TransientClass, float>, 5> candidates{{
        {TransientClass::Kick, kickScore},
        {TransientClass::Snare, snareScore},
        {TransientClass::HiHat, hihatScore},
        {TransientClass::TonalOnset, tonalScore},
        {TransientClass::Percussion, percussionScore},
    }};

    TransientClass best      = TransientClass::Unclassified;
    float          bestScore = 0.0f;
    for (const auto& [klass, score] : candidates) {
        if (score > bestScore) {
            bestScore = score;
            best      = klass;
        }
    }

    if (bestScore >= config.minConfidence) {
        return Classification{best, bestScore};
    }
    // Honest "doesn't match" (doc: "reported honestly rather than forced into a bucket") — the
    // confidence reflects how clearly nothing matched, not a guess at a class.
    return Classification{TransientClass::Unclassified, std::clamp(1.0f - bestScore, 0.0f, 1.0f)};
}

}  // namespace aud::transients

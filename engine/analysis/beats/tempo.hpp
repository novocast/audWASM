#pragma once

// M13's tempo estimation ("Decision — autocorrelation of the ODF, weighted by a log-Gaussian
// tempo prior, with explicit octave-ambiguity reporting"). Pure functions over an already-computed
// ODF series — no PCM access, cheap enough to call per-window for the tempo-variation series.

#include <cstddef>
#include <span>
#include <vector>

namespace aud::beats {

struct TempoConfig {
    double minBpm = 40.0;
    double maxBpm = 240.0;

    double priorCenterBpm     = 120.0;
    double priorSigmaOctaves  = 0.8;

    // Alternatives within this fraction of the primary's score are reported (doc: "Octave
    // alternatives are always offered when the x2/÷2 score is within 30% of the primary" — this
    // threshold governs which local maxima make the `alternatives` list at all, independent of
    // that specific acceptance check, which the caller verifies against the returned scores).
    double alternativeScoreFraction = 0.3;
    std::size_t maxAlternatives      = 5;

    double windowSeconds = 10.0;  // for the tempo-variation series
};

struct TempoCandidate {
    double bpm   = 0.0;
    float  score = 0.0f;  // prior-weighted harmonic-sum autocorrelation score, arbitrary units
};

struct TempoEstimate {
    double                       primaryBpm      = 0.0;
    float                        tempoConfidence = 0.0f;  // 0..1, peakiness of the scored curve
    std::vector<TempoCandidate>  alternatives;             // includes the primary, sorted by score desc
};

// Autocorrelates `odf` over the lag range implied by [minBpm, maxBpm], applies a harmonic-sum
// (comb-filter) step so a tempo and its multiples reinforce, then weights by a log-normal prior
// centred at `priorCenterBpm`. Returns the best candidate plus runners-up (importantly including
// the x2/÷2/x1.5 alternatives when they're close — see TempoConfig::alternativeScoreFraction).
[[nodiscard]] TempoEstimate estimateTempo(std::span<const float> odf, double hopSeconds, TempoConfig config = {});

struct TempoSeriesResult {
    std::vector<float> tempoSeries;    // primary BPM per `windowSeconds` window
    bool               tempoIsStable = true;
};

// Runs estimateTempo() independently over consecutive `windowSeconds` windows and reports whether
// the result is roughly constant (programmed music) or varies (live performance) — doc: "a track
// with a varying tempo should not be given a single confident BPM."
[[nodiscard]] TempoSeriesResult estimateTempoSeries(std::span<const float> odf, double hopSeconds,
                                                       TempoConfig config = {});

}  // namespace aud::beats

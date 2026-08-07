#pragma once

// M13's streaming Analyzer, tying odf.{hpp,cpp}, whitening.{hpp,cpp}, normalise.{hpp,cpp},
// peak_pick.{hpp,cpp}, tempo.{hpp,cpp} and beat_tracker.{hpp,cpp} together — see
// documentation/tasks/M13-beat-detection.md for the pipeline and design rationale.
//
// Non-owning-target design, mirroring DcAnalyzer/StatisticsAnalyzer/ClipDetectorAnalyzer: the
// caller owns a plain-data BeatResult and this analyser only ever writes into it. Concrete
// analyser types are never named outside aud_core's own translation units (see dc_analyzer.hpp's
// header comment) — go through makeBeatAnalyzer().
//
// Runs its own StftProcessor pass (fftSize 2048 / hop 512 by default, matching the doc). Sharing
// that pass with other analysers that also need an STFT (e.g. a future spectral-feature module) is
// M20 registry's job, not this milestone's — see DC's analogous note about M09's mean.
//
// v1 emits beats only (doc's "Downbeat detection — deferred, designed for"): `Beat::beatIndexInBar`
// is always -1 unless a manual edit sets a downbeat, but the field exists from day one so adding
// automatic downbeat detection later is additive, not a schema change.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../util/audio_types.hpp"
#include "../analyzer.hpp"
#include "odf.hpp"
#include "peak_pick.hpp"
#include "tempo.hpp"
#include "beat_tracker.hpp"

namespace aud::beats {

struct Onset {
    double        timeSeconds = 0.0;
    FrameIndex    frame        = kNoFrame;
    float         strength     = 0.0f;   // normalised ODF value at the peak
    std::uint8_t  bandMask     = 0;       // which frequency bands contributed (bit0=low bit1=mid bit2=high)
};

struct Beat {
    double        timeSeconds     = 0.0;
    FrameIndex    frame            = kNoFrame;
    float         confidence       = 0.0f;
    std::int32_t  beatIndexInBar  = -1;   // -1 == unknown; 0 == downbeat
};

struct BeatParameters {
    std::size_t fftSize = 2048;
    std::size_t hopSize = 512;
    OdfConfig          odf;
    PeakPickConfig      peakPick;
    TempoConfig         tempo;
    BeatTrackerConfig   beatTracker;
    int                 timeSignatureBeatsPerBar = 4;
};

using BeatConfig = BeatParameters;

struct BeatResult {
    std::vector<Onset> onsets;
    std::vector<Beat>  beats;

    double primaryBpm      = 0.0;
    float  tempoConfidence = 0.0f;  // 0..1 — peakiness of the tempo autocorrelation
    float  phaseConfidence = 0.0f;  // 0..1 — how well beats align with strong ODF peaks

    std::vector<TempoCandidate> alternatives;   // includes the primary, sorted by score desc
    std::vector<float>          tempoSeries;     // primary BPM per ~10s window
    bool                        tempoIsStable = true;

    std::vector<float> odf;             // normalised combined ODF, retained for visualisation
    double              odfHopSeconds = 0.0;

    BeatParameters parametersUsed;

    [[nodiscard]] std::string toJson() const;
};

// Manual edits, stored separately from detected values so a re-analysis doesn't destroy them (doc:
// "Editability" — nudge phase, set tempo manually, mark a downbeat, add/remove beats, tap tempo).
// M16's cache distinguishes detected vs manual the same way M12's correction preview never
// overwrites raw measurement — see DcAnalyzer's header comment for the analogous split.
struct BeatEdits {
    std::optional<double> tempoOverrideBpm;
    double                  phaseNudgeSeconds = 0.0;   // added to every beat's time
    std::optional<double>  downbeatTimeSeconds;         // nearest beat to this time becomes beatIndexInBar==0
    std::vector<double>    addedBeatSeconds;
    std::vector<double>    removedBeatSeconds;           // beats within a tolerance of these are dropped
    int                    timeSignatureBeatsPerBar = 0;  // 0 == keep detected value
};

// Applies `edits` on top of a detected `BeatResult`, without mutating it — merged at query time
// (doc's "merged at query time"), so cache round-trips and re-analysis never need to know about
// edits at all.
[[nodiscard]] BeatResult applyManualEdits(const BeatResult& detected, const BeatEdits& edits);

class BeatAnalyzer final : public Analyzer {
public:
    explicit BeatAnalyzer(BeatResult& result, BeatConfig config = {});
    ~BeatAnalyzer() override;

    [[nodiscard]] std::string_view id() const noexcept override { return "analysis.beats"; }
    [[nodiscard]] std::uint32_t    version() const noexcept override { return 1; }

    Result<void>           begin(const AudioSpec& spec) override;
    Result<void>           process(const ChunkView& chunk) override;
    Result<AnalysisResult> finish() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    BeatResult* m_result;
    BeatConfig  m_config;
};

[[nodiscard]] std::unique_ptr<Analyzer> makeBeatAnalyzer(BeatResult& result, BeatConfig config = {});

}  // namespace aud::beats

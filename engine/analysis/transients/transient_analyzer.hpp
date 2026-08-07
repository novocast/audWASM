#pragma once

// M14's transient detector — ties refine_timing.{hpp,cpp}, features.{hpp,cpp}, classifier.{hpp,cpp},
// click_detector.{hpp,cpp} and dropout_detector.{hpp,cpp} together. See
// documentation/tasks/M14-transient-detection.md for the pipeline and design rationale.
//
// **Consumes M13's onset list as candidates; does not run a second onset detector** (doc: "How this
// differs from M13" / risk table's "Duplicating M13's onset detection"). Decoupled from
// aud::beats::Onset the same way SilenceInput is decoupled from M08/M09's result types
// (silence_detector.hpp's header comment) — callers (the Embind boundary, tests) build a
// TransientCandidate list from whichever onset result they already have.
//
// Shape: unlike BeatAnalyzer (a pure STFT-consumer that never needs raw PCM again once each frame
// is processed), M14's actual new work — sample-accurate timing, LPC click residuals, dropout runs
// — all need *random access* to raw PCM around arbitrary points, not just an in-order stream. This
// analyser therefore buffers the whole track's mono mixdown across process() calls (bounded by
// track length; the earlier streaming analysers avoid this because they don't need it) and does all
// its real work in finish(), where the buffer is complete. Still conforms to the Analyzer interface
// (M00 §6) so it composes with progressive decode the same way the others do.
//
// Non-owning-target design, mirroring BeatAnalyzer/DcAnalyzer/StatisticsAnalyzer: the caller owns a
// plain-data TransientResult and this analyser only ever writes into it.
//
// v1 analyses the mono mixdown only (same decision as BeatAnalyzer, for the same reason: per-channel
// transient detection is out of scope for v1) — every Transient's `channel` field is kAllChannels.

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../../util/audio_types.hpp"
#include "../analyzer.hpp"
#include "classifier.hpp"
#include "click_detector.hpp"
#include "dropout_detector.hpp"
#include "features.hpp"
#include "refine_timing.hpp"

namespace aud::transients {

// A channel value meaning "not specific to any one channel" — see silence/clipping's per-event
// `channel` fields for the general convention this follows; v1 always uses this (header comment
// above).
inline constexpr ChannelIndex kAllChannels = std::numeric_limits<ChannelIndex>::max();

// What M14 needs from M13, decoupled from aud::beats::Onset (header comment above).
struct TransientCandidate {
    double timeSeconds = 0.0;
    float  strength    = 0.0f;  // carried through to Transient::strength
};

struct TransientParameters {
    RefineTimingConfig    refineTiming;
    FeatureConfig          features;
    ClassifierConfig        classifier;
    ClickDetectorConfig     clickDetector;
    DropoutDetectorConfig   dropoutDetector;
    double                  onsetCoincidenceMs = 5.0;  // click-vs-onset rejection tolerance
};

using TransientConfig = TransientParameters;

struct Transient {
    FrameIndex     startFrame  = kNoFrame;  // zero crossing before the attack
    FrameIndex     attackFrame = kNoFrame;  // steepest rise
    double         startSeconds  = 0.0;
    double         attackSeconds = 0.0;
    ChannelIndex   channel        = kAllChannels;
    TransientClass classification = TransientClass::Unclassified;
    float          classConfidence = 0.0f;
    float          strength       = 0.0f;  // normalised; from the M13 candidate, or 1.0 for defects
    float          peakDbfs       = -std::numeric_limits<float>::infinity();
    float          attackTimeMs   = 0.0f;
    float          decayTimeMs    = 0.0f;
    float          spectralCentroidHz = 0.0f;
    float          spectralFlatness   = 0.0f;
    std::array<float, 4> bandEnergyRatio{};
};

struct TransientResult {
    std::vector<Transient> transients;                                    // musical transients only
    std::array<std::uint32_t, kTransientClassCount> countByClass{};       // indexed by TransientClass
    std::vector<Transient> defects;                                       // Click + Dropout, surfaced separately

    TransientParameters parametersUsed;

    [[nodiscard]] std::string toJson() const;
};

class TransientAnalyzer final : public Analyzer {
public:
    explicit TransientAnalyzer(TransientResult& result, std::vector<TransientCandidate> candidates,
                                TransientConfig config = {});
    ~TransientAnalyzer() override;

    [[nodiscard]] std::string_view id() const noexcept override { return "analysis.transients"; }
    [[nodiscard]] std::uint32_t    version() const noexcept override { return 1; }

    Result<void>           begin(const AudioSpec& spec) override;
    Result<void>           process(const ChunkView& chunk) override;
    Result<AnalysisResult> finish() override;

private:
    TransientResult*                 m_result;
    std::vector<TransientCandidate>  m_candidates;
    TransientConfig                  m_config;

    std::vector<Sample> m_mono;  // whole-track mono mixdown, appended to by process() (header comment above)
    SampleRate           m_sampleRate = 0;
};

[[nodiscard]] std::unique_ptr<Analyzer> makeTransientAnalyzer(TransientResult& result,
                                                                 std::vector<TransientCandidate> candidates,
                                                                 TransientConfig config = {});

}  // namespace aud::transients

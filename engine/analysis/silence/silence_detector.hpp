#pragma once

// M10's parameterised silence detector — see documentation/tasks/M10-silence-detection.md.
//
// This is a pure function over already-computed series, not a streaming Analyzer: M10's
// "re-running on parameter change" decision requires the whole detection pass to be cheap enough
// to run on every slider tick (O(windows), sub-millisecond for an hour of audio), which re-reading
// PCM cannot be. It operates on:
//   - M09's retained 50ms windowed-RMS series (threshold mode) plus an optional per-window
//     "all samples exactly zero" flag alongside it (digital mode) — both interleaved by channel
//     exactly like StatisticsResult::rmsSeries, so callers just copy those fields across.
//   - M08's momentary loudness series (perceptual mode, gated at the R128 absolute gate).
//
// Sample-accurate boundary work needs PCM and is therefore a separate, deliberately optional step
// — see boundary_refine.hpp. detect() alone is enough to show window-grid-precision markers live
// while a user drags a threshold slider; refinement runs once, debounced, after they let go.
//
// Order of operations is fixed and NOT commutative (M10 "Order of operations"):
//   1. Threshold each window (with hysteresis) -> boolean run-length encoding, per channel.
//   2. Combine channels per channelMode -> one boolean per window.
//   3. Merge silent runs separated by non-silent gaps shorter than mergeGapMs.
//   4. Discard silent runs shorter than minDurationMs.
//   5. Classify: touching window 0 -> leading; touching the last window -> trailing; both -> the
//      whole file; otherwise internal.
// Digital and perceptual modes reuse the same pipeline against a different level series and
// threshold; digital additionally disables hysteresis (there is nothing to hover around an exact
// zero test).

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "../../util/audio_types.hpp"

namespace aud::silence {

enum class ChannelMode : std::uint8_t { All, Any };

struct SilenceParameters {
    double      thresholdDb   = -60.0;  // RMS threshold, dBFS (threshold mode only)
    double      minDurationMs = 500.0;
    double      mergeGapMs    = 100.0;
    ChannelMode channelMode   = ChannelMode::All;  // All: silent only if every channel is silent
    bool        useHysteresis = true;
    double      hysteresisDb  = 3.0;  // exit threshold = thresholdDb + hysteresisDb
};

enum class SilenceKind : std::uint8_t { Digital, Threshold, Perceptual };
enum class SilencePosition : std::uint8_t { Leading, Internal, Trailing, EntireFile };

struct SilenceRegion {
    FrameRange range;  // window-grid precision until boundary_refine.hpp runs
    double     startSeconds = 0.0;
    double     endSeconds   = 0.0;

    SilenceKind     kind     = SilenceKind::Threshold;
    SilencePosition position = SilencePosition::Internal;

    // "How quiet was it really." Filled from the coarse window series by detect() (approximate —
    // see silence_detector.cpp); overwritten with sample-accurate values by
    // refineRegionLevels()/refineRegionBoundaries() in boundary_refine.hpp once PCM is available.
    // For SilenceKind::Perceptual there is no separate LUFS field in the M10 spec, so this holds
    // mean momentary LUFS within the region instead of a dBFS RMS value; peakDbfsWithin is unused
    // (left at -inf) for that kind since "peak" isn't a loudness-domain notion.
    double peakDbfsWithin = -std::numeric_limits<double>::infinity();
    double rmsDbfsWithin  = -std::numeric_limits<double>::infinity();

    std::uint32_t channelMask = 0;  // bit c set => channel c was silent at some point in this region
};

struct SilenceResult {
    std::vector<SilenceRegion> regions;
    double leadingSilenceSeconds  = 0.0;  // 0 if none
    double trailingSilenceSeconds = 0.0;
    double totalSilenceSeconds    = 0.0;
    double silenceFraction        = 0.0;  // 0..1, of frameCount
    SilenceParameters parametersUsed;      // echoed back — results are meaningless without them

    // Stable JSON serialisation for the CLI (`aud_cli --silence`) and bug reports, matching
    // StatisticsResult::toJson()'s convention (docs/report-schema.json covers this shape too).
    [[nodiscard]] std::string toJson() const;
};

// Everything the detector needs from M09/M08, decoupled from those modules' own result types so
// this header has no dependency on analysis/statistics or analysis/loudness. Callers (the Embind
// boundary, tests) build this from whichever result structs they already have.
struct SilenceInput {
    // 50ms-resolution RMS series (linear, not dB), interleaved exactly like
    // StatisticsResult::rmsSeries: [ch0_w0, ch1_w0, ch0_w1, ch1_w1, ...].
    std::vector<float> rmsSeries;
    std::uint32_t      channelCount = 0;

    // Same interleaving/window grid as rmsSeries; empty if the M09 pass didn't flag all-zero
    // windows (detectDigital() then reports no regions rather than guessing).
    std::vector<std::uint8_t> digitalSilenceSeries;

    // M08's momentary loudness, LUFS, ~100ms resolution, already a single (K-weighted,
    // channel-summed) series — perceptual mode has no per-channel notion.
    std::vector<float> momentaryLufs;
    double              momentaryLufsWindowSeconds = 0.1;

    SampleRate sampleRate    = 0;
    FrameIndex frameCount    = 0;
    double     rmsWindowSeconds = 0.05;  // seconds per rmsSeries/digitalSilenceSeries window (M09: 50ms)

    [[nodiscard]] std::size_t rmsWindowCount() const noexcept {
        return channelCount == 0 ? 0 : rmsSeries.size() / channelCount;
    }
};

class SilenceDetector {
public:
    // Default mode described in the M10 doc: RMS windows vs thresholdDb, with hysteresis.
    [[nodiscard]] static SilenceResult detectThreshold(const SilenceInput& input, const SilenceParameters& params);

    // Exact all-zero windows. minDurationMs/mergeGapMs/channelMode still apply; thresholdDb and
    // hysteresis do not (there's nothing to hover around an exact test). Returns an empty result
    // if input.digitalSilenceSeries is empty.
    [[nodiscard]] static SilenceResult detectDigital(const SilenceInput& input, const SilenceParameters& params);

    // Same pipeline over M08's momentary loudness series, gated at `gateLufs` (R128 absolute gate,
    // -70 LUFS by default per M08). Returns an empty result if input.momentaryLufs is empty.
    [[nodiscard]] static SilenceResult detectPerceptual(const SilenceInput& input, const SilenceParameters& params,
                                                         double gateLufs = -70.0);
};

}  // namespace aud::silence

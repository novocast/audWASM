#pragma once

// M08's streaming Analyzer: ties k_weighting, channel_weights, block_accumulator, gating, lra and
// true_peak together behind the M00 §6 Analyzer shape. begin() derives per-rate K-weighting
// coefficients and resolves channel weights; process() K-weights each channel, feeds 100 ms
// sub-blocks into the accumulator (which fans out into the momentary/short-term/LRA sliding
// windows), and separately runs the unweighted signal through the true-peak oversampler; finish()
// runs the two-stage integrated gate and the LRA percentile method.
//
// Silence must read NaN/-infinity, never 0 (M08: "a silent file reading '0 LUFS' is a bug report
// waiting to happen") — every gate and every log() call here already returns -infinity/NaN for a
// non-positive input by construction (see gating.cpp's loudnessFromMeanSquare), so no special-case
// branch is needed here; a pure-silence file simply produces empty gatingBlocks and the gate
// itself reports NaN.

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "../analyzer.hpp"
#include "block_accumulator.hpp"
#include "channel_weights.hpp"
#include "k_weighting.hpp"
#include "true_peak.hpp"

namespace aud::loudness {

struct LoudnessConfig {
    // 4 (spec-compliant minimum), 8 or 16 ("high precision" mode; always reported alongside the
    // number so a 4x reading is never silently mistaken for a more accurate one, per M08).
    std::uint32_t truePeakOversampling = 4;

    // WAVE_FORMAT_EXTENSIBLE-style channel mask, when the container supplies one. Not yet wired
    // up from decode_session/wav_decoder (M02 gap documented in channel_weights.hpp) — left here
    // so callers with their own mask can pass it through without waiting on that wiring.
    std::optional<std::uint32_t> wavChannelMask;
};

struct LoudnessResult {
    double integratedLufs  = std::numeric_limits<double>::quiet_NaN();  // NaN if nothing passed the gate
    double loudnessRangeLu = std::numeric_limits<double>::quiet_NaN();
    double truePeakDbtp    = -std::numeric_limits<double>::infinity();  // max over channels
    double samplePeakDbfs  = -std::numeric_limits<double>::infinity();

    std::vector<double> truePeakPerChannelDbtp;
    std::vector<double> samplePeakPerChannelDbfs;

    FrameIndex    truePeakFrame        = kNoFrame;
    std::uint32_t truePeakOversampling = 4;

    // 100 ms-resolution time series (M08: "we get a loudness graph over time for free").
    std::vector<float> momentaryLufs;
    std::vector<float> shortTermLufs;

    // Surfaced to the UI per channel_weights.hpp's "never silently guess" rule: true whenever no
    // explicit container layout mask was available and the documented channel-count fallback was
    // used instead.
    bool usedFallbackChannelLayout = false;

    // Gain to reach `targetLufs` (e.g. -14 for Spotify/YouTube/Amazon, -23 for EBU R128
    // broadcast) — information, never applied automatically (M08: "show the gain delta; never
    // apply it").
    [[nodiscard]] double gainToTargetDb(double targetLufs) const noexcept { return targetLufs - integratedLufs; }
};

// Non-owning-target design, deliberately mirroring waveform_analyzer.hpp's WaveformAnalyzer/
// WaveformStore split: the caller owns a plain-data LoudnessResult (no vtable, so no RTTI-boundary
// concern) and this analyser only ever writes into it. That keeps every concrete polymorphic
// LoudnessAnalyzer instance entirely inside aud_core's own -fno-rtti-flagged translation unit —
// consumers outside it (tests, the Embind bindings target, which is RTTI-enabled) interact only
// through the returned `Analyzer*` from makeLoudnessAnalyzer() and their own LoudnessResult,
// never by naming or destroying a LoudnessAnalyzer directly. See WaveformAnalyzer's factory
// comment for why that direct-construction path is a link-time trap, not just a style preference.
class LoudnessAnalyzer final : public Analyzer {
public:
    // Non-owning: `result` must outlive this analyser and is overwritten in place by finish().
    // Not noexcept: the sliding-window members below allocate their ring buffers here.
    LoudnessAnalyzer(LoudnessResult& result, LoudnessConfig config = {})
        : m_result(&result), m_config(config) {}

    [[nodiscard]] std::string_view id() const noexcept override { return "analysis.loudness"; }
    [[nodiscard]] std::uint32_t    version() const noexcept override { return 1; }

    Result<void>           begin(const AudioSpec& spec) override;
    Result<void>           process(const ChunkView& chunk) override;
    Result<AnalysisResult> finish() override;

private:
    void onSubBlock(double weightedMeanSquare);

    LoudnessResult* m_result;
    LoudnessConfig  m_config;
    SampleRate      m_sampleRate = 0;
    ChannelIndex    m_channels   = 0;

    std::vector<KWeightingFilter> m_filters;
    std::vector<double>           m_channelWeights;
    BlockAccumulator               m_blockAccumulator;

    SlidingWindowSum m_momentaryWindow{4};
    SlidingWindowSum m_shortTermWindow{30};
    SlidingWindowSum m_lraWindow{30};

    // Linear (pre-log) channel-weighted mean squares, kept separately from the dB time series
    // above because gating/LRA must average in the power domain (see gating.hpp).
    std::vector<double> m_gatingBlocks;  // one per 400 ms momentary block, for integrated gating
    std::vector<double> m_lraBlocks;     // one per 1 s hop over the LRA short-term window

    std::unique_ptr<TruePeakMeter> m_truePeak;

    std::vector<std::vector<double>> m_kWeightedScratch;  // per-channel, reused across process() calls
};

// Factory, mirroring waveform_analyzer.hpp's makeWaveformAnalyzer: keeps `new` inside aud_core's
// own -fno-rtti-flagged translation unit so consumers outside it (tests, Embind bindings) never
// need to name the concrete type across that boundary. `result` must outlive the returned
// Analyzer and every chunk passed to it.
[[nodiscard]] std::unique_ptr<Analyzer> makeLoudnessAnalyzer(LoudnessResult& result, LoudnessConfig config = {});

}  // namespace aud::loudness

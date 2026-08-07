#pragma once

// M09's streaming Analyzer: the single-pass numeric summary of a file — peaks, RMS, distribution,
// dynamics, stereo relationship. Ties accumulator.hpp (per-channel), stereo.hpp (cross-channel),
// bit_depth.hpp and dynamic_range.hpp together behind the M00 Sec6 Analyzer shape, mirroring
// LoudnessAnalyzer's (M08) non-owning-result design: the caller owns a plain-data
// StatisticsResult and this analyser only ever writes into it.
//
// "Dynamic range" is intentionally reported as three separately labelled numbers (crest factor,
// TT-style DR, and a cross-link to M08's LRA) rather than one ambiguous figure — see M09 §"The
// windowed RMS and the dynamic range question". They will disagree; that's expected.

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../analyzer.hpp"
#include "accumulator.hpp"
#include "bit_depth.hpp"
#include "histogram.hpp"
#include "stereo.hpp"

namespace aud::statistics {

struct ChannelStatistics {
    double     peak        = 0.0;
    double     peakDbfs    = -std::numeric_limits<double>::infinity();
    FrameIndex peakFrame   = kNoFrame;

    double minValue = 0.0;
    double maxValue = 0.0;

    double dcOffset = 0.0;  // mean(x) — feeds M12

    double rms     = 0.0;
    double rmsDbfs = -std::numeric_limits<double>::infinity();

    double variance = 0.0;
    double stdDev    = 0.0;

    double crestFactorDb = 0.0;

    double zeroCrossingRate = 0.0;  // crossings per second

    BitDepthResult bitDepth;

    std::array<std::uint32_t, kHistogramBuckets> histogram{};
};

struct StatisticsConfig {
    // Source integer container bit depth (e.g. 16, 24), or 0 for float sources — see
    // decoder::StreamInfo::bitDepth (M02). Not derivable from the Analyzer's AudioSpec alone, so
    // callers must pass it explicitly.
    std::uint32_t containerBitDepth = 0;
};

struct StatisticsResult {
    std::vector<ChannelStatistics> channels;
    std::optional<StereoStatistics> stereo;

    // 50ms, per channel, interleaved: [ch0_w0, ch1_w0, ch0_w1, ch1_w1, ...] — see M09's report
    // struct sketch.
    std::vector<float> rmsSeries;
    std::uint32_t       rmsSeriesChannelCount = 0;

    // Same interleaving/grid as rmsSeries: 1 if every sample in that channel's window was exactly
    // zero, 0 otherwise. Feeds M10's digital-silence mode without a second pass over the PCM.
    std::vector<std::uint8_t> allZeroSeries;

    double dynamicRangeDr = 0.0;  // TT-style, overall (mean across channels)
    double crestFactorDb  = 0.0;  // overall

    SampleRate   sampleRate = 0;
    ChannelIndex channelCount = 0;
    FrameIndex   frameCount   = 0;

    // Stable, versioned JSON serialisation — see docs/report-schema.json (M09: "the JSON report is
    // a stable, versioned public artifact").
    [[nodiscard]] std::string toJson() const;
};

// Non-owning-target design, mirroring LoudnessAnalyzer/WaveformAnalyzer — see loudness_analyzer.hpp
// for why the concrete type must stay inside aud_core's own -fno-rtti translation unit.
class StatisticsAnalyzer final : public Analyzer {
public:
    // Non-owning: `result` must outlive this analyser and is overwritten in place by finish().
    explicit StatisticsAnalyzer(StatisticsResult& result, StatisticsConfig config = {})
        : m_result(&result), m_config(config) {}

    [[nodiscard]] std::string_view id() const noexcept override { return "analysis.statistics"; }
    [[nodiscard]] std::uint32_t    version() const noexcept override { return 1; }

    Result<void>           begin(const AudioSpec& spec) override;
    Result<void>           process(const ChunkView& chunk) override;
    Result<AnalysisResult> finish() override;

private:
    StatisticsResult* m_result;
    StatisticsConfig  m_config;
    SampleRate        m_sampleRate = 0;
    ChannelIndex      m_channels   = 0;

    std::vector<ChannelAccumulator>  m_accumulators;
    std::vector<BitDepthAccumulator> m_bitDepthAccumulators;
    std::optional<StereoAccumulator> m_stereo;
};

// Factory, mirroring makeLoudnessAnalyzer/makeWaveformAnalyzer.
[[nodiscard]] std::unique_ptr<Analyzer> makeStatisticsAnalyzer(StatisticsResult& result, StatisticsConfig config = {});

}  // namespace aud::statistics

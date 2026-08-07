#pragma once

// M12's DC offset analyzer — see documentation/tasks/M12-dc-offset-detection.md.
//
// A streaming Analyzer, like M11's ClipDetectorAnalyzer: classifying drifting/sectional DC needs a
// windowed series at 1s resolution, which only exists by walking PCM chunk-by-chunk, so this can't
// be a pure function over an already-reduced series the way M10's SilenceDetector is. The *global*
// figure per channel is exactly M09's ChannelAccumulator::mean() (mean(x)); this analyser tracks its
// own running sum/count for it rather than threading a second call through StatisticsAnalyzer —
// cross-analyzer sharing inside a single decode pass is M20's registry's job, not this milestone's,
// and M11's own peak tracking already duplicates the same trivial part of M09's accumulator for the
// same reason (see M12's risk table, "Duplicating M09's mean computation").
//
// Never applies a correction to the analysis buffer (M12 "the engine computes correction
// *parameters* and previewed *metrics*; it never writes corrected audio in v1" / M02 "no gain
// applied on decode"). Everything below is read-only measurement plus arithmetic on already-known
// scalars (peak/min/max) — correction preview never re-reads PCM.

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../../util/audio_types.hpp"
#include "../analyzer.hpp"

namespace aud::dc {

enum class DcPattern : std::uint8_t { None, Constant, Drifting, Sectional };

struct ChannelDcResult {
    double offsetLinear  = 0.0;                                       // mean(x), signed
    double offsetDbfs    = -std::numeric_limits<double>::infinity();  // 20*log10(|offsetLinear|)
    double offsetPercent = 0.0;                                       // offsetLinear * 100

    DcPattern pattern = DcPattern::None;

    // Min/max of the 1s windowed series (M12 "Output"); the regression guard for a global mean
    // that hides a sectional or drifting file — see the risk table.
    double minWindowOffset = 0.0;
    double maxWindowOffset = 0.0;

    double headroomLostDb           = 0.0;                                       // 20*log10(1/(1-|dc|))
    double peakAfterCorrectionDbfs  = -std::numeric_limits<double>::infinity();  // analytic, no second pass
    double recommendedHighpassHz    = 0.0;  // 0 if a constant subtraction suffices (M12: Constant/None/Sectional)

    std::vector<FrameIndex> stepLocations;  // populated only for Sectional
};

// Only knob exposed today: the significance threshold, configurable per M12's "Decision — default
// 'significant' threshold is -60 dBFS, configurable". Window length (1s) is fixed by the design.
struct DcConfig {
    double significanceThresholdDbfs = -60.0;
};

struct DcOffsetResult {
    std::vector<ChannelDcResult> channels;

    // 1s-resolution windowed mean, interleaved by channel exactly like StatisticsResult::rmsSeries:
    // [ch0_w0, ch1_w0, ch0_w1, ch1_w1, ...] (M12 "Decision — also compute a windowed DC series").
    std::vector<float> windowSeries;
    std::uint32_t       windowSeriesChannelCount = 0;
    double               windowSeconds            = 1.0;

    double significanceThresholdDbfs = -60.0;  // echoed from DcConfig — results are meaningless without it
    bool   anySignificant             = false;  // true if any channel's pattern != None

    // Stable JSON serialisation, matching StatisticsResult::toJson()'s convention.
    [[nodiscard]] std::string toJson() const;
};

// Non-owning-target design, mirroring StatisticsAnalyzer/ClipDetectorAnalyzer: the caller owns a
// plain-data DcOffsetResult and this analyser only ever writes into it.
class DcAnalyzer final : public Analyzer {
public:
    explicit DcAnalyzer(DcOffsetResult& result, DcConfig config = {});
    ~DcAnalyzer() override;

    [[nodiscard]] std::string_view id() const noexcept override { return "analysis.dc"; }
    [[nodiscard]] std::uint32_t    version() const noexcept override { return 1; }

    Result<void>           begin(const AudioSpec& spec) override;
    Result<void>           process(const ChunkView& chunk) override;
    Result<AnalysisResult> finish() override;

private:
    struct ChannelState;  // per-channel accumulator, defined in the .cpp

    DcOffsetResult* m_result;
    DcConfig        m_config;
    SampleRate      m_sampleRate   = 0;
    ChannelIndex    m_channels     = 0;
    std::uint64_t   m_windowFrames = 0;
    FrameIndex      m_frameCursor  = 0;

    std::vector<ChannelState> m_channelStates;
};

// Factory, mirroring makeStatisticsAnalyzer/makeClipDetectorAnalyzer.
[[nodiscard]] std::unique_ptr<Analyzer> makeDcAnalyzer(DcOffsetResult& result, DcConfig config = {});

}  // namespace aud::dc

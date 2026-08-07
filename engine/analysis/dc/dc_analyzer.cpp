#include "dc_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <utility>

namespace aud::dc {

namespace {
constexpr double kNegInf = -std::numeric_limits<double>::infinity();
constexpr double kPosInf = std::numeric_limits<double>::infinity();

// Midpoint of M12's "typically 5-20 Hz, 2nd-order Butterworth" recommendation for drifting DC.
constexpr double kRecommendedHighpassHz = 10.0;

double linearToDbfs(double linear) noexcept { return linear > 0.0 ? 20.0 * std::log10(linear) : kNegInf; }

std::string jsonNumber(double v) {
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return v > 0 ? "1e999" : "-1e999";  // JSON has no Infinity; sentinel large magnitude
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

const char* patternName(DcPattern pattern) {
    switch (pattern) {
        case DcPattern::None:      return "none";
        case DcPattern::Constant:  return "constant";
        case DcPattern::Drifting:  return "drifting";
        case DcPattern::Sectional: return "sectional";
    }
    return "unknown";
}

struct PatternClassification {
    DcPattern                 pattern = DcPattern::None;
    std::vector<std::size_t> stepWindowIndices;  // index i means the step falls between window i and i+1
};

// Classification per M12's table: None/Constant/Drifting/Sectional. Sectional is detected first
// (a cheap threshold on the first difference, per M12: "a cheap CUSUM or a threshold on the first
// difference after smoothing") because a single dominant step and a gradual, uniform trend look
// identical if you only look at total range — the discriminator is whether one difference is an
// outlier against the rest, not merely large.
PatternClassification classifyPattern(const std::vector<float>& series, double thresholdLinear) {
    PatternClassification out;
    if (series.empty()) return out;

    const std::size_t n = series.size();

    bool anySignificant = false;
    for (float v : series) {
        if (std::fabs(static_cast<double>(v)) >= thresholdLinear) {
            anySignificant = true;
            break;
        }
    }
    if (!anySignificant) return out;  // None: |DC| below threshold everywhere

    if (n == 1) {
        out.pattern = DcPattern::Constant;
        return out;
    }

    // Step detection: an outlier in the |first difference| series, large relative both to the
    // typical window-to-window change and to the significance threshold itself. A single dominant
    // step and a gradual, uniform trend can have the same total range, so mean/stddev (which the
    // step itself would drag upward) isn't a robust discriminator — median + MAD is: one step among
    // many near-identical diffs barely moves either statistic, per M12: "a cheap CUSUM or a
    // threshold on the first difference after smoothing".
    std::vector<double> absDiffs(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        absDiffs[i] = std::fabs(static_cast<double>(series[i + 1]) - static_cast<double>(series[i]));
    }

    std::vector<double> sortedDiffs = absDiffs;
    std::sort(sortedDiffs.begin(), sortedDiffs.end());
    const double medianDiff = sortedDiffs[sortedDiffs.size() / 2];

    std::vector<double> deviations(absDiffs.size());
    for (std::size_t i = 0; i < absDiffs.size(); ++i) deviations[i] = std::fabs(absDiffs[i] - medianDiff);
    std::sort(deviations.begin(), deviations.end());
    const double mad = deviations[deviations.size() / 2];

    // 1.4826 makes MAD a consistent estimator of stddev for normal-ish data. The floor of half the
    // significance threshold matters for a clean, near-zero-variance ramp: float32 rounding alone
    // can make consecutive diffs differ by ~1e-8, which a bare "> median" test (mad ~ 0) would
    // mistake for a step; a real edit-point jump is expected to be on the order of the significance
    // threshold itself, comfortably clearing this floor.
    const double outlierThreshold = medianDiff + std::max(3.0 * 1.4826 * mad, 0.5 * thresholdLinear);

    for (std::size_t i = 0; i < absDiffs.size(); ++i) {
        const bool isOutlier     = absDiffs[i] > outlierThreshold;
        const bool isSubstantial = absDiffs[i] >= thresholdLinear;
        if (isOutlier && isSubstantial) out.stepWindowIndices.push_back(i);
    }

    if (!out.stepWindowIndices.empty()) {
        out.pattern = DcPattern::Sectional;
        return out;
    }

    // Drifting: a strong monotonic (Pearson) correlation between window index and window value,
    // spanning a range that's itself significant.
    double sumIdx = 0.0, sumIdxSq = 0.0, sumVal = 0.0, sumValSq = 0.0, sumIdxVal = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        const double y = static_cast<double>(series[i]);
        sumIdx += x;
        sumIdxSq += x * x;
        sumVal += y;
        sumValSq += y * y;
        sumIdxVal += x * y;
    }
    const double nD    = static_cast<double>(n);
    const double covXY = sumIdxVal / nD - (sumIdx / nD) * (sumVal / nD);
    const double varX  = sumIdxSq / nD - (sumIdx / nD) * (sumIdx / nD);
    const double varY  = sumValSq / nD - (sumVal / nD) * (sumVal / nD);
    const double correlation = (varX > 0.0 && varY > 0.0) ? covXY / std::sqrt(varX * varY) : 0.0;

    double minV = series[0], maxV = series[0];
    for (float v : series) {
        minV = std::min<double>(minV, v);
        maxV = std::max<double>(maxV, v);
    }
    const double range = maxV - minV;

    if (std::fabs(correlation) >= 0.8 && range >= thresholdLinear) {
        out.pattern = DcPattern::Drifting;
        return out;
    }

    out.pattern = DcPattern::Constant;
    return out;
}

}  // namespace

// One channel's running state: a plain sum/count for the global mean and min/max/peak (all M09
// duplicates trivially rather than shares, per this file's header comment), plus a 1s window
// accumulator that produces the windowed series M09 doesn't compute.
struct DcAnalyzer::ChannelState {
    double        sumX  = 0.0;
    std::uint64_t n     = 0;
    double        minValue = std::numeric_limits<double>::infinity();
    double        maxValue = -std::numeric_limits<double>::infinity();

    double        windowSum   = 0.0;
    std::uint64_t windowCount = 0;

    std::vector<float> windowSeries;

    void process(std::span<const Sample> samples, std::uint64_t windowFrames) {
        for (Sample s : samples) {
            const double v = static_cast<double>(s);
            sumX += v;
            ++n;
            if (v < minValue) minValue = v;
            if (v > maxValue) maxValue = v;

            windowSum += v;
            ++windowCount;
            if (windowFrames > 0 && windowCount >= windowFrames) flushWindow();
        }
    }

    void flushWindow() {
        if (windowCount == 0) return;
        windowSeries.push_back(static_cast<float>(windowSum / static_cast<double>(windowCount)));
        windowSum   = 0.0;
        windowCount = 0;
    }

    void finish() { flushWindow(); }

    [[nodiscard]] double mean() const noexcept { return n == 0 ? 0.0 : sumX / static_cast<double>(n); }
};

DcAnalyzer::DcAnalyzer(DcOffsetResult& result, DcConfig config) : m_result(&result), m_config(config) {}

DcAnalyzer::~DcAnalyzer() = default;

Result<void> DcAnalyzer::begin(const AudioSpec& spec) {
    if (spec.sampleRate == 0 || spec.channels == 0) {
        return Error{ErrorCode::InvalidArgument, "analysis.dc",
                      "DcAnalyzer requires a non-zero sample rate and channel count"};
    }

    m_sampleRate   = spec.sampleRate;
    m_channels     = spec.channels;
    m_windowFrames = static_cast<std::uint64_t>(m_sampleRate);  // 1s windows, per M12's design
    m_frameCursor  = 0;

    m_channelStates.assign(m_channels, ChannelState{});

    *m_result                        = DcOffsetResult{};
    m_result->windowSeriesChannelCount = m_channels;
    m_result->significanceThresholdDbfs = m_config.significanceThresholdDbfs;

    return {};
}

Result<void> DcAnalyzer::process(const ChunkView& chunk) {
    const std::size_t frameCount = chunk.frameCount();
    if (frameCount == 0) return {};

    for (ChannelIndex ch = 0; ch < m_channels && ch < chunk.channels.size(); ++ch) {
        m_channelStates[ch].process(chunk.channels[ch], m_windowFrames);
    }

    m_frameCursor += static_cast<FrameIndex>(frameCount);
    return {};
}

Result<AnalysisResult> DcAnalyzer::finish() {
    for (auto& state : m_channelStates) state.finish();

    const double thresholdLinear = std::pow(10.0, m_config.significanceThresholdDbfs / 20.0);

    std::size_t maxSeriesLen = 0;
    for (const auto& state : m_channelStates) maxSeriesLen = std::max(maxSeriesLen, state.windowSeries.size());

    m_result->channels.clear();
    m_result->channels.reserve(m_channels);

    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        const auto& state = m_channelStates[ch];

        ChannelDcResult cr;
        cr.offsetLinear  = state.mean();
        cr.offsetDbfs    = linearToDbfs(std::fabs(cr.offsetLinear));
        cr.offsetPercent = cr.offsetLinear * 100.0;

        const auto classification = classifyPattern(state.windowSeries, thresholdLinear);
        cr.pattern = classification.pattern;

        if (state.windowSeries.empty()) {
            cr.minWindowOffset = cr.offsetLinear;
            cr.maxWindowOffset = cr.offsetLinear;
        } else {
            cr.minWindowOffset = *std::min_element(state.windowSeries.begin(), state.windowSeries.end());
            cr.maxWindowOffset = *std::max_element(state.windowSeries.begin(), state.windowSeries.end());
        }

        const double absDc = std::fabs(cr.offsetLinear);
        const double denom = 1.0 - absDc;
        cr.headroomLostDb  = denom > 0.0 ? 20.0 * std::log10(1.0 / denom) : kPosInf;

        // Analytic correction preview (M12 "Preview metrics ... computed analytically"): subtracting
        // a constant shifts min and max by exactly that constant — no second pass over PCM.
        const double minValue = state.n == 0 ? 0.0 : state.minValue;
        const double maxValue = state.n == 0 ? 0.0 : state.maxValue;
        const double correctedMin = minValue - cr.offsetLinear;
        const double correctedMax = maxValue - cr.offsetLinear;
        cr.peakAfterCorrectionDbfs = linearToDbfs(std::max(std::fabs(correctedMin), std::fabs(correctedMax)));

        cr.recommendedHighpassHz = cr.pattern == DcPattern::Drifting ? kRecommendedHighpassHz : 0.0;

        if (cr.pattern == DcPattern::Sectional) {
            cr.stepLocations.reserve(classification.stepWindowIndices.size());
            for (std::size_t idx : classification.stepWindowIndices) {
                // The step falls between window idx and idx+1; report the frame at the boundary.
                cr.stepLocations.push_back(static_cast<FrameIndex>((idx + 1) * m_windowFrames));
            }
        }

        m_result->channels.push_back(cr);
    }

    // Interleaved window series (same convention as StatisticsResult::rmsSeries): channels can
    // differ in length by at most one trailing partial window.
    m_result->windowSeries.assign(maxSeriesLen * m_channels, 0.0f);
    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        const auto& series = m_channelStates[ch].windowSeries;
        for (std::size_t i = 0; i < series.size(); ++i) {
            m_result->windowSeries[i * m_channels + ch] = series[i];
        }
    }

    m_result->anySignificant =
        std::any_of(m_result->channels.begin(), m_result->channels.end(),
                    [](const ChannelDcResult& c) { return c.pattern != DcPattern::None; });

    return AnalysisResult{};
}

std::unique_ptr<Analyzer> makeDcAnalyzer(DcOffsetResult& result, DcConfig config) {
    return std::make_unique<DcAnalyzer>(result, config);
}

std::string DcOffsetResult::toJson() const {
    std::ostringstream out;
    out << "{\"schemaVersion\":\"1.0.0\",";
    out << "\"significanceThresholdDbfs\":" << jsonNumber(significanceThresholdDbfs) << ",";
    out << "\"anySignificant\":" << (anySignificant ? "true" : "false") << ",";
    out << "\"windowSeconds\":" << jsonNumber(windowSeconds) << ",";

    out << "\"channels\":[";
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (i > 0) out << ",";
        const auto& c = channels[i];
        out << "{";
        out << "\"offsetLinear\":" << jsonNumber(c.offsetLinear) << ",";
        out << "\"offsetDbfs\":" << jsonNumber(c.offsetDbfs) << ",";
        out << "\"offsetPercent\":" << jsonNumber(c.offsetPercent) << ",";
        out << "\"pattern\":\"" << patternName(c.pattern) << "\",";
        out << "\"minWindowOffset\":" << jsonNumber(c.minWindowOffset) << ",";
        out << "\"maxWindowOffset\":" << jsonNumber(c.maxWindowOffset) << ",";
        out << "\"headroomLostDb\":" << jsonNumber(c.headroomLostDb) << ",";
        out << "\"peakAfterCorrectionDbfs\":" << jsonNumber(c.peakAfterCorrectionDbfs) << ",";
        out << "\"recommendedHighpassHz\":" << jsonNumber(c.recommendedHighpassHz) << ",";
        out << "\"stepLocations\":[";
        for (std::size_t s = 0; s < c.stepLocations.size(); ++s) {
            if (s > 0) out << ",";
            out << c.stepLocations[s];
        }
        out << "]";
        out << "}";
    }
    out << "],";

    out << "\"windowSeriesChannelCount\":" << windowSeriesChannelCount << ",";
    out << "\"windowSeries\":[";
    for (std::size_t i = 0; i < windowSeries.size(); ++i) {
        if (i > 0) out << ",";
        out << jsonNumber(windowSeries[i]);
    }
    out << "]";

    out << "}";
    return out.str();
}

}  // namespace aud::dc

#include "statistics_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

#include "dynamic_range.hpp"

namespace aud::statistics {

namespace {
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double linearToDbfs(double linear) {
    return linear <= 0.0 ? kNegInf : 20.0 * std::log10(linear);
}

// Minimal JSON escaping — only the characters that can appear in our own static strings need it,
// but this covers the general case cheaply enough to not worry about it.
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default:   out += c; break;
        }
    }
    return out;
}

std::string jsonNumber(double v) {
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return v > 0 ? "1e999" : "-1e999";  // JSON has no Infinity; sentinel large magnitude
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}
}  // namespace

Result<void> StatisticsAnalyzer::begin(const AudioSpec& spec) {
    if (spec.sampleRate == 0 || spec.channels == 0) {
        return Error{ErrorCode::InvalidArgument, "analysis.statistics",
                      "StatisticsAnalyzer requires a non-zero sample rate and channel count"};
    }

    m_sampleRate = spec.sampleRate;
    m_channels   = spec.channels;

    m_accumulators.assign(m_channels, ChannelAccumulator{});
    for (auto& acc : m_accumulators) acc.begin(m_sampleRate);

    m_bitDepthAccumulators.assign(m_channels, BitDepthAccumulator{});
    for (auto& acc : m_bitDepthAccumulators) acc.begin(m_config.containerBitDepth);

    if (m_channels == 2) {
        m_stereo.emplace();
        m_stereo->begin(m_sampleRate);
    } else {
        m_stereo.reset();
    }

    *m_result                = StatisticsResult{};
    m_result->sampleRate     = m_sampleRate;
    m_result->channelCount   = m_channels;
    m_result->rmsSeriesChannelCount = m_channels;

    return {};
}

Result<void> StatisticsAnalyzer::process(const ChunkView& chunk) {
    const std::size_t frameCount = chunk.frameCount();
    if (frameCount == 0) return {};

    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        m_accumulators[ch].process(chunk.channels[ch], chunk.startFrame);
        m_bitDepthAccumulators[ch].process(chunk.channels[ch]);
    }

    if (m_stereo.has_value() && m_channels >= 2) {
        m_stereo->process(chunk.channels[0], chunk.channels[1]);
    }

    m_result->frameCount += static_cast<FrameIndex>(frameCount);

    return {};
}

Result<AnalysisResult> StatisticsAnalyzer::finish() {
    for (auto& acc : m_accumulators) acc.finish();
    if (m_stereo.has_value()) m_stereo->finish();

    double sumSqTotal      = 0.0;
    std::uint64_t nTotal   = 0;
    double        peakMax  = 0.0;
    double        drSum    = 0.0;
    std::size_t   drCount  = 0;

    m_result->channels.clear();
    m_result->channels.reserve(m_channels);

    std::size_t maxSeriesLen = 0;
    for (auto& acc : m_accumulators) maxSeriesLen = std::max(maxSeriesLen, acc.rmsSeries().size());

    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        const auto& acc = m_accumulators[ch];

        ChannelStatistics cs;
        cs.peak      = acc.peak();
        cs.peakDbfs  = linearToDbfs(cs.peak);
        cs.peakFrame = acc.peakFrame();
        cs.minValue  = acc.minValue();
        cs.maxValue  = acc.maxValue();
        cs.dcOffset  = acc.mean();
        cs.rms       = acc.rms();
        cs.rmsDbfs   = linearToDbfs(cs.rms);
        cs.variance  = acc.variance();
        cs.stdDev    = acc.stdDev();
        cs.crestFactorDb = cs.rms <= 0.0 ? 0.0 : 20.0 * std::log10(cs.peak / cs.rms);

        const double durationSeconds =
            m_sampleRate == 0 ? 0.0 : static_cast<double>(acc.sampleCount()) / static_cast<double>(m_sampleRate);
        cs.zeroCrossingRate = durationSeconds <= 0.0 ? 0.0 : static_cast<double>(acc.zeroCrossings()) / durationSeconds;

        cs.bitDepth  = m_bitDepthAccumulators[ch].finish();
        cs.histogram = acc.histogram().buckets();

        sumSqTotal += acc.sumXSquares();
        nTotal     += acc.sampleCount();
        peakMax     = std::max(peakMax, cs.peak);

        const double dr = computeDynamicRangeDb(acc.blockRms(), acc.blockPeaks());
        if (!std::isnan(dr) && !std::isinf(dr)) {
            drSum += dr;
            ++drCount;
        }

        m_result->channels.push_back(cs);
    }

    // Interleaved RMS series (M09 report shape): [ch0_w0, ch1_w0, ch0_w1, ch1_w1, ...]. Channels'
    // series can differ in length by at most one trailing partial window; pad the shorter with its
    // last value's window-of-silence (0) rather than misaligning the interleave.
    m_result->rmsSeries.assign(maxSeriesLen * m_channels, 0.0f);
    // Padding windows (see comment above) read as digitally silent — accurate, since there's no
    // sample there at all.
    m_result->allZeroSeries.assign(maxSeriesLen * m_channels, 1);
    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        const auto& series = m_accumulators[ch].rmsSeries();
        const auto& allZero = m_accumulators[ch].allZeroSeries();
        for (std::size_t i = 0; i < series.size(); ++i) {
            m_result->rmsSeries[i * m_channels + ch] = series[i];
        }
        for (std::size_t i = 0; i < allZero.size(); ++i) {
            m_result->allZeroSeries[i * m_channels + ch] = allZero[i];
        }
    }

    const double rmsOverall = nTotal == 0 ? 0.0 : std::sqrt(sumSqTotal / static_cast<double>(nTotal));
    m_result->crestFactorDb = rmsOverall <= 0.0 ? 0.0 : 20.0 * std::log10(peakMax / rmsOverall);
    m_result->dynamicRangeDr = drCount == 0 ? 0.0 : drSum / static_cast<double>(drCount);

    if (m_stereo.has_value() && m_channels == 2) {
        const auto& left  = m_accumulators[0];
        const auto& right = m_accumulators[1];
        m_result->stereo = m_stereo->computeStatistics(left.mean(), left.variance(), left.sumXSquares(), right.mean(),
                                                         right.variance(), right.sumXSquares());
    }

    return AnalysisResult{};
}

std::unique_ptr<Analyzer> makeStatisticsAnalyzer(StatisticsResult& result, StatisticsConfig config) {
    return std::make_unique<StatisticsAnalyzer>(result, config);
}

std::string StatisticsResult::toJson() const {
    std::ostringstream out;
    // Schema versioned per docs/report-schema.json (M09: "additive changes only within a major").
    out << "{\"schemaVersion\":\"1.0.0\",";
    out << "\"sampleRate\":" << sampleRate << ",";
    out << "\"channelCount\":" << channelCount << ",";
    out << "\"frameCount\":" << frameCount << ",";
    out << "\"crestFactorDb\":" << jsonNumber(crestFactorDb) << ",";
    out << "\"dynamicRangeDr\":" << jsonNumber(dynamicRangeDr) << ",";

    out << "\"channels\":[";
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (i > 0) out << ",";
        const auto& c = channels[i];
        out << "{";
        out << "\"peak\":" << jsonNumber(c.peak) << ",";
        out << "\"peakDbfs\":" << jsonNumber(c.peakDbfs) << ",";
        out << "\"peakFrame\":" << c.peakFrame << ",";
        out << "\"minValue\":" << jsonNumber(c.minValue) << ",";
        out << "\"maxValue\":" << jsonNumber(c.maxValue) << ",";
        out << "\"dcOffset\":" << jsonNumber(c.dcOffset) << ",";
        out << "\"rms\":" << jsonNumber(c.rms) << ",";
        out << "\"rmsDbfs\":" << jsonNumber(c.rmsDbfs) << ",";
        out << "\"variance\":" << jsonNumber(c.variance) << ",";
        out << "\"stdDev\":" << jsonNumber(c.stdDev) << ",";
        out << "\"crestFactorDb\":" << jsonNumber(c.crestFactorDb) << ",";
        out << "\"zeroCrossingRate\":" << jsonNumber(c.zeroCrossingRate) << ",";
        out << "\"effectiveBitDepth\":"
            << (c.bitDepth.effectiveBitDepth.has_value() ? std::to_string(*c.bitDepth.effectiveBitDepth) : "null")
            << ",";
        out << "\"containerBitDepth\":" << c.bitDepth.containerBitDepth << ",";
        out << "\"ditherLikely\":" << (c.bitDepth.ditherLikely ? "true" : "false") << ",";
        out << "\"ditherConfidence\":" << jsonNumber(c.bitDepth.ditherConfidence) << ",";
        out << "\"bitDepthDescription\":\"" << jsonEscape(c.bitDepth.describe()) << "\",";
        out << "\"histogram\":[";
        for (std::size_t b = 0; b < c.histogram.size(); ++b) {
            if (b > 0) out << ",";
            out << c.histogram[b];
        }
        out << "]";
        out << "}";
    }
    out << "],";

    out << "\"stereo\":";
    if (stereo.has_value()) {
        out << "{";
        out << "\"correlation\":" << jsonNumber(stereo->correlation) << ",";
        out << "\"balanceDb\":" << jsonNumber(stereo->balanceDb) << ",";
        out << "\"monoCompatibilityDb\":" << jsonNumber(stereo->monoCompatibilityDb) << ",";
        out << "\"correlationSeries\":[";
        for (std::size_t i = 0; i < stereo->correlationSeries.size(); ++i) {
            if (i > 0) out << ",";
            out << jsonNumber(stereo->correlationSeries[i]);
        }
        out << "]";
        out << "}";
    } else {
        out << "null";
    }
    out << ",";

    out << "\"rmsSeriesChannelCount\":" << rmsSeriesChannelCount << ",";
    out << "\"rmsSeries\":[";
    for (std::size_t i = 0; i < rmsSeries.size(); ++i) {
        if (i > 0) out << ",";
        out << jsonNumber(rmsSeries[i]);
    }
    out << "]";

    out << "}";
    return out.str();
}

}  // namespace aud::statistics

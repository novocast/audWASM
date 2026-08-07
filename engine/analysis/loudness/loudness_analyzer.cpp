#include "loudness_analyzer.hpp"

#include <algorithm>
#include <limits>

#include "gating.hpp"
#include "lra.hpp"

namespace aud::loudness {

namespace {
constexpr double kNegInf = -std::numeric_limits<double>::infinity();
}  // namespace

Result<void> LoudnessAnalyzer::begin(const AudioSpec& spec) {
    if (spec.sampleRate == 0 || spec.channels == 0) {
        return Error{ErrorCode::InvalidArgument, "analysis.loudness",
                      "LoudnessAnalyzer requires a non-zero sample rate and channel count"};
    }

    m_sampleRate = spec.sampleRate;
    m_channels   = spec.channels;

    m_filters.assign(m_channels, KWeightingFilter{});
    for (auto& filter : m_filters) filter.configure(m_sampleRate);

    const ChannelWeightResolution resolved = m_config.wavChannelMask.has_value()
        ? resolveChannelRolesFromWavMask(m_channels, *m_config.wavChannelMask)
        : resolveChannelRolesByCount(m_channels);
    m_channelWeights = resolved.weights;

    m_blockAccumulator.begin(m_sampleRate, m_channelWeights);

    m_momentaryWindow = SlidingWindowSum(4);
    m_shortTermWindow = SlidingWindowSum(30);
    m_lraWindow        = SlidingWindowSum(30);

    m_gatingBlocks.clear();
    m_lraBlocks.clear();

    m_truePeak = std::make_unique<TruePeakMeter>(m_sampleRate, m_channels, m_config.truePeakOversampling);

    m_kWeightedScratch.assign(m_channels, {});

    *m_result = LoudnessResult{};
    m_result->truePeakOversampling      = m_config.truePeakOversampling;
    m_result->usedFallbackChannelLayout = resolved.usedFallback;
    m_result->truePeakPerChannelDbtp.assign(m_channels, kNegInf);
    m_result->samplePeakPerChannelDbfs.assign(m_channels, kNegInf);

    return {};
}

void LoudnessAnalyzer::onSubBlock(double weightedMeanSquare) {
    double momentaryMean = 0.0;
    if (m_momentaryWindow.push(weightedMeanSquare, momentaryMean)) {
        m_result->momentaryLufs.push_back(static_cast<float>(loudnessFromMeanSquare(momentaryMean)));
        m_gatingBlocks.push_back(momentaryMean);
    }

    double shortTermMean = 0.0;
    if (m_shortTermWindow.push(weightedMeanSquare, shortTermMean)) {
        m_result->shortTermLufs.push_back(static_cast<float>(loudnessFromMeanSquare(shortTermMean)));
    }

    double lraMean = 0.0;
    if (m_lraWindow.push(weightedMeanSquare, lraMean)) {
        // Tech 3342's own 1 s hop over its 3 s window — distinct from the meter's 100 ms hop
        // above, hence the separate sampling here rather than reusing every completed value.
        if (m_lraWindow.pushCount() % 10 == 0) {
            m_lraBlocks.push_back(lraMean);
        }
    }
}

Result<void> LoudnessAnalyzer::process(const ChunkView& chunk) {
    const std::size_t frameCount = chunk.frameCount();
    if (frameCount == 0) return {};

    std::vector<std::span<const double>> kWeightedSpans(m_channels);
    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        m_kWeightedScratch[ch].resize(frameCount);
        m_filters[ch].process(chunk.channels[ch], m_kWeightedScratch[ch]);
        kWeightedSpans[ch] = m_kWeightedScratch[ch];
    }

    m_blockAccumulator.process(kWeightedSpans, [this](double z) { onSubBlock(z); });

    m_truePeak->process(chunk.channels);

    return {};
}

Result<AnalysisResult> LoudnessAnalyzer::finish() {
    m_blockAccumulator.finish();
    m_truePeak->finish();

    const GateResult gate    = gateIntegratedLoudness(m_gatingBlocks);
    m_result->integratedLufs = gate.integratedLufs;
    m_result->loudnessRangeLu = computeLoudnessRange(m_lraBlocks);

    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        m_result->truePeakPerChannelDbtp[ch]   = m_truePeak->truePeakDbtpFor(ch);
        m_result->samplePeakPerChannelDbfs[ch] = m_truePeak->samplePeakDbfsFor(ch);
    }
    m_result->truePeakDbtp   = m_truePeak->truePeakDbtpOverall();
    m_result->samplePeakDbfs = m_result->samplePeakPerChannelDbfs.empty()
        ? kNegInf
        : *std::max_element(m_result->samplePeakPerChannelDbfs.begin(), m_result->samplePeakPerChannelDbfs.end());
    m_result->truePeakFrame = m_truePeak->truePeakFrame();

    return AnalysisResult{};
}

std::unique_ptr<Analyzer> makeLoudnessAnalyzer(LoudnessResult& result, LoudnessConfig config) {
    return std::make_unique<LoudnessAnalyzer>(result, config);
}

}  // namespace aud::loudness

#include "transient_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace aud::transients {

namespace {

std::string jsonNumber(double v) {
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return v > 0 ? "1e999" : "-1e999";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

float linearToDbfs(float linear) {
    constexpr float kFloor = 1e-9f;
    return 20.0f * std::log10(std::max(std::fabs(linear), kFloor));
}

const char* classToString(TransientClass klass) {
    switch (klass) {
        case TransientClass::Kick: return "kick";
        case TransientClass::Snare: return "snare";
        case TransientClass::HiHat: return "hiHat";
        case TransientClass::Percussion: return "percussion";
        case TransientClass::TonalOnset: return "tonalOnset";
        case TransientClass::Click: return "click";
        case TransientClass::Dropout: return "dropout";
        case TransientClass::Unclassified: return "unclassified";
    }
    return "unclassified";
}

void appendTransientJson(std::ostringstream& out, const Transient& t) {
    out << "{\"startSeconds\":" << jsonNumber(t.startSeconds) << ",\"attackSeconds\":" << jsonNumber(t.attackSeconds)
        << ",\"classification\":\"" << classToString(t.classification) << "\""
        << ",\"classConfidence\":" << jsonNumber(t.classConfidence) << ",\"strength\":" << jsonNumber(t.strength)
        << ",\"peakDbfs\":" << jsonNumber(t.peakDbfs) << ",\"attackTimeMs\":" << jsonNumber(t.attackTimeMs)
        << ",\"decayTimeMs\":" << jsonNumber(t.decayTimeMs)
        << ",\"spectralCentroidHz\":" << jsonNumber(t.spectralCentroidHz)
        << ",\"spectralFlatness\":" << jsonNumber(t.spectralFlatness) << ",\"bandEnergyRatio\":["
        << jsonNumber(t.bandEnergyRatio[0]) << "," << jsonNumber(t.bandEnergyRatio[1]) << ","
        << jsonNumber(t.bandEnergyRatio[2]) << "," << jsonNumber(t.bandEnergyRatio[3]) << "]}";
}

}  // namespace

TransientAnalyzer::TransientAnalyzer(TransientResult& result, std::vector<TransientCandidate> candidates,
                                      TransientConfig config)
    : m_result(&result), m_candidates(std::move(candidates)), m_config(config) {}

TransientAnalyzer::~TransientAnalyzer() = default;

Result<void> TransientAnalyzer::begin(const AudioSpec& spec) {
    if (spec.sampleRate == 0 || spec.channels == 0) {
        return Error{ErrorCode::InvalidArgument, "analysis.transients",
                      "TransientAnalyzer requires a non-zero sample rate and channel count"};
    }

    m_sampleRate = spec.sampleRate;
    m_mono.clear();
    if (spec.frameCount != kNoFrame && spec.frameCount > 0) {
        m_mono.reserve(static_cast<std::size_t>(spec.frameCount));
    }

    *m_result                = TransientResult{};
    m_result->parametersUsed = m_config;

    return {};
}

Result<void> TransientAnalyzer::process(const ChunkView& chunk) {
    const std::size_t frameCount = chunk.frameCount();
    if (frameCount == 0) return {};

    const std::size_t channelCount = chunk.channels.size();
    const std::size_t base         = m_mono.size();
    m_mono.resize(base + frameCount, 0.0f);
    for (std::size_t ch = 0; ch < channelCount; ++ch) {
        const auto& channelSamples = chunk.channels[ch];
        for (std::size_t i = 0; i < frameCount; ++i) m_mono[base + i] += channelSamples[i];
    }
    if (channelCount > 1) {
        const float inv = 1.0f / static_cast<float>(channelCount);
        for (std::size_t i = base; i < m_mono.size(); ++i) m_mono[i] *= inv;
    }

    return {};
}

Result<AnalysisResult> TransientAnalyzer::finish() {
    if (m_mono.empty() || m_sampleRate == 0) return AnalysisResult{};

    auto bumpCount = [this](TransientClass klass) {
        m_result->countByClass[static_cast<std::size_t>(klass)]++;
    };

    // --- Musical transients: refine each M13 candidate's timing, extract features, classify. ---
    m_result->transients.reserve(m_candidates.size());
    for (const TransientCandidate& candidate : m_candidates) {
        const auto approxFrame = static_cast<FrameIndex>(
            std::llround(candidate.timeSeconds * static_cast<double>(m_sampleRate)));

        const RefinedTiming refined = refineTransientTiming(m_mono, m_sampleRate, approxFrame, m_config.refineTiming);
        if (refined.attackFrame == kNoFrame) continue;

        const std::size_t attackIndex  = static_cast<std::size_t>(refined.attackFrame);
        const std::size_t windowLength = m_mono.size() - attackIndex;
        const SpectralFeatures features =
            computeSpectralFeatures(std::span<const Sample>(m_mono).subspan(attackIndex, windowLength), m_sampleRate,
                                     m_config.features);

        ClassificationInput classInput;
        classInput.features      = features;
        classInput.attackTimeMs  = refined.attackTimeMs;
        classInput.decayTimeMs   = refined.decayTimeMs;
        classInput.peakAmplitude = refined.peakAmplitude;
        const Classification classification = classifyTransient(classInput, m_config.classifier);

        Transient t;
        t.startFrame    = refined.startFrame;
        t.attackFrame   = refined.attackFrame;
        t.startSeconds  = static_cast<double>(refined.startFrame) / static_cast<double>(m_sampleRate);
        t.attackSeconds = static_cast<double>(refined.attackFrame) / static_cast<double>(m_sampleRate);
        t.classification    = classification.klass;
        t.classConfidence   = classification.confidence;
        t.strength           = candidate.strength;
        t.peakDbfs           = linearToDbfs(refined.peakAmplitude);
        t.attackTimeMs       = refined.attackTimeMs;
        t.decayTimeMs        = refined.decayTimeMs;
        t.spectralCentroidHz = features.spectralCentroidHz;
        t.spectralFlatness   = features.spectralFlatness;
        t.bandEnergyRatio    = features.bandEnergyRatio;

        bumpCount(t.classification);
        m_result->transients.push_back(t);
    }

    // --- Defects: clicks (LPC residual, onset-coincidence rejected) and dropouts. ---
    std::vector<double> onsetTimesSeconds;
    onsetTimesSeconds.reserve(m_candidates.size());
    for (const TransientCandidate& candidate : m_candidates) onsetTimesSeconds.push_back(candidate.timeSeconds);

    auto clickCandidates = detectClicks(m_mono, m_sampleRate, m_config.clickDetector);
    clickCandidates =
        rejectOnsetCoincidences(std::move(clickCandidates), m_sampleRate, onsetTimesSeconds, m_config.onsetCoincidenceMs);

    m_result->defects.reserve(clickCandidates.size());
    for (const ClickCandidate& click : clickCandidates) {
        Transient t;
        t.startFrame    = click.frame;
        t.attackFrame   = click.frame;
        t.startSeconds  = static_cast<double>(click.frame) / static_cast<double>(m_sampleRate);
        t.attackSeconds = t.startSeconds;
        t.classification  = TransientClass::Click;
        t.classConfidence = std::clamp(click.residualRatio / static_cast<float>(m_config.clickDetector.madMultiplier * 2.0), 0.0f, 1.0f);
        t.strength         = 1.0f;
        t.peakDbfs         = static_cast<std::size_t>(click.frame) < m_mono.size()
                                    ? linearToDbfs(m_mono[static_cast<std::size_t>(click.frame)])
                                    : -std::numeric_limits<float>::infinity();

        bumpCount(t.classification);
        m_result->defects.push_back(t);
    }

    const auto dropoutRuns = detectDropouts(m_mono, m_sampleRate, m_config.dropoutDetector);
    m_result->defects.reserve(m_result->defects.size() + dropoutRuns.size());
    for (const DropoutRun& run : dropoutRuns) {
        Transient t;
        t.startFrame    = run.begin;
        t.attackFrame   = run.begin;
        t.startSeconds  = static_cast<double>(run.begin) / static_cast<double>(m_sampleRate);
        t.attackSeconds = t.startSeconds;
        t.classification  = TransientClass::Dropout;
        t.classConfidence = 1.0f;
        t.strength         = 1.0f;
        t.decayTimeMs      = static_cast<float>((run.end - run.begin) * 1000.0 / static_cast<double>(m_sampleRate));
        t.peakDbfs         = -std::numeric_limits<float>::infinity();

        bumpCount(t.classification);
        m_result->defects.push_back(t);
    }

    return AnalysisResult{};
}

std::unique_ptr<Analyzer> makeTransientAnalyzer(TransientResult& result, std::vector<TransientCandidate> candidates,
                                                 TransientConfig config) {
    return std::make_unique<TransientAnalyzer>(result, std::move(candidates), config);
}

std::string TransientResult::toJson() const {
    std::ostringstream out;
    out << "{\"schemaVersion\":\"1.0.0\",";

    out << "\"countByClass\":[";
    for (std::size_t i = 0; i < countByClass.size(); ++i) {
        if (i > 0) out << ",";
        out << countByClass[i];
    }
    out << "],";

    out << "\"transients\":[";
    for (std::size_t i = 0; i < transients.size(); ++i) {
        if (i > 0) out << ",";
        appendTransientJson(out, transients[i]);
    }
    out << "],";

    out << "\"defects\":[";
    for (std::size_t i = 0; i < defects.size(); ++i) {
        if (i > 0) out << ",";
        appendTransientJson(out, defects[i]);
    }
    out << "]";

    out << "}";
    return out.str();
}

}  // namespace aud::transients

#include "beat_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "../../fft/stft.hpp"
#include "normalise.hpp"
#include "whitening.hpp"

namespace aud::beats {

namespace {

std::string jsonNumber(double v) {
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return v > 0 ? "1e999" : "-1e999";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

}  // namespace

struct BeatAnalyzer::Impl {
    aud::fft::StftProcessor stft;
    OdfComputer               odfComputer;
    SpectralWhitener          whitener;

    std::vector<Sample> monoScratch;
    std::vector<float>  magnitudeScratch;

    // Each ODF component's raw series, kept separate — combination happens once at finish(), after
    // each is normalised against its own whole-track median/MAD (see odf.hpp's header comment for
    // why this replaced per-frame online normalisation).
    std::vector<float>        fluxSeries;
    std::vector<float>        complexSeries;
    std::vector<float>        hfcSeries;
    std::vector<std::uint8_t> bandMasks;

    SampleRate sampleRate = 0;
    double     hopSeconds = 0.0;

    Impl(aud::fft::StftProcessor stftProcessor, std::size_t binCount, SampleRate sr, const OdfConfig& odfConfig)
        : stft(std::move(stftProcessor)), odfComputer(binCount, sr, odfConfig), whitener(binCount) {}
};

BeatAnalyzer::BeatAnalyzer(BeatResult& result, BeatConfig config) : m_result(&result), m_config(config) {}

BeatAnalyzer::~BeatAnalyzer() = default;

Result<void> BeatAnalyzer::begin(const AudioSpec& spec) {
    if (spec.sampleRate == 0 || spec.channels == 0) {
        return Error{ErrorCode::InvalidArgument, "analysis.beats",
                      "BeatAnalyzer requires a non-zero sample rate and channel count"};
    }

    aud::fft::StftConfig stftConfig;
    stftConfig.fftSize = m_config.fftSize;
    stftConfig.hopSize = m_config.hopSize;
    stftConfig.scaling = aud::fft::SpectrumScaling::Raw;  // ODF applies its own log-compression
    stftConfig.centered = true;                             // M06's centring guard — see beat_analyzer.hpp

    AUD_TRY_ASSIGN(stft, aud::fft::StftProcessor::create(stftConfig, spec.sampleRate));

    const std::size_t binCount = stft.binCount();
    m_impl = std::make_unique<Impl>(std::move(stft), binCount, spec.sampleRate, m_config.odf);
    m_impl->sampleRate = spec.sampleRate;
    m_impl->hopSeconds  = static_cast<double>(m_config.hopSize) / static_cast<double>(spec.sampleRate);

    *m_result                = BeatResult{};
    m_result->odfHopSeconds  = m_impl->hopSeconds;
    m_result->parametersUsed = m_config;

    return {};
}

Result<void> BeatAnalyzer::process(const ChunkView& chunk) {
    const std::size_t frameCount = chunk.frameCount();
    if (frameCount == 0 || !m_impl) return {};

    // Mono mixdown — the ODF pipeline operates on a single spectrum per frame (doc's pipeline
    // diagram takes one PCM stream in); per-channel ODFs are out of scope for v1.
    m_impl->monoScratch.assign(frameCount, 0.0f);
    const std::size_t channelCount = chunk.channels.size();
    for (std::size_t ch = 0; ch < channelCount; ++ch) {
        const auto& channelSamples = chunk.channels[ch];
        for (std::size_t i = 0; i < frameCount; ++i) {
            m_impl->monoScratch[i] += channelSamples[i];
        }
    }
    if (channelCount > 1) {
        const float inv = 1.0f / static_cast<float>(channelCount);
        for (Sample& s : m_impl->monoScratch) s *= inv;
    }

    auto& impl = *m_impl;
    auto  callback = [&impl](const aud::fft::StftFrame& frame) {
        impl.magnitudeScratch.assign(frame.bins.begin(), frame.bins.end());
        impl.whitener.apply(impl.magnitudeScratch);

        const OdfSample sample = impl.odfComputer.push(impl.magnitudeScratch, frame.complexBins);
        impl.fluxSeries.push_back(sample.flux);
        impl.complexSeries.push_back(sample.complexDomain);
        impl.hfcSeries.push_back(sample.hfc);
        impl.bandMasks.push_back(sample.bandMask);
    };

    AUD_TRY(m_impl->stft.process(m_impl->monoScratch, callback));
    return {};
}

Result<AnalysisResult> BeatAnalyzer::finish() {
    if (!m_impl) return AnalysisResult{};

    auto& impl = *m_impl;

    auto callback = [&impl](const aud::fft::StftFrame& frame) {
        impl.magnitudeScratch.assign(frame.bins.begin(), frame.bins.end());
        impl.whitener.apply(impl.magnitudeScratch);

        const OdfSample sample = impl.odfComputer.push(impl.magnitudeScratch, frame.complexBins);
        impl.fluxSeries.push_back(sample.flux);
        impl.complexSeries.push_back(sample.complexDomain);
        impl.hfcSeries.push_back(sample.hfc);
        impl.bandMasks.push_back(sample.bandMask);
    };
    AUD_TRY(impl.stft.finish(callback));

    NormaliseConfig normaliseConfig;
    // ~1s window regardless of the configured hop (doc: "moving median/MAD ... over ~1 s").
    normaliseConfig.windowFrames =
        impl.hopSeconds > 0.0 ? std::max<std::size_t>(1, static_cast<std::size_t>(1.0 / impl.hopSeconds)) : 86;

    // Normalise each ODF component against its own whole-track median/MAD *before* combining —
    // see odf.hpp's header comment: combining first and normalising the sum (or normalising each
    // online, frame by frame) both let one component's scale carry history across occurrences of
    // the same transient, which measurably biased onset timing on periodic material.
    const std::vector<float> fluxNorm    = normaliseOdf(impl.fluxSeries, normaliseConfig);
    const std::vector<float> complexNorm = normaliseOdf(impl.complexSeries, normaliseConfig);
    const std::vector<float> hfcNorm     = normaliseOdf(impl.hfcSeries, normaliseConfig);

    const float weightSum = m_config.odf.fluxWeight + m_config.odf.complexWeight + m_config.odf.hfcWeight;
    std::vector<float> normalisedOdf(fluxNorm.size(), 0.0f);
    for (std::size_t i = 0; i < normalisedOdf.size(); ++i) {
        normalisedOdf[i] = weightSum > 0.0f
                                 ? (m_config.odf.fluxWeight * fluxNorm[i] + m_config.odf.complexWeight * complexNorm[i] +
                                    m_config.odf.hfcWeight * hfcNorm[i]) /
                                       weightSum
                                 : 0.0f;
    }

    m_result->odf = normalisedOdf;

    // --- Onsets ---
    // No group-delay compensation needed here: an earlier version of this pipeline (each ODF
    // component online-normalised frame by frame before combining, see odf.hpp's header comment)
    // had a measurable, history-dependent timing bias on periodic material. Normalising each
    // component's *whole* series before combining (immediately above) removed it — verified
    // directly against isolated and periodic impulse trains at the default fftSize/hopSize, which
    // now land within ~1-2ms of the true transient once past the very first ODF frame or two (a
    // one-off edge effect at the very start of a track, not a systematic bias).
    const auto peaks = pickPeaks(normalisedOdf, impl.hopSeconds, m_config.peakPick);
    m_result->onsets.reserve(peaks.size());
    for (const auto& p : peaks) {
        Onset onset;
        const double refinedFrame = static_cast<double>(p.frameIndex) + p.subFrameOffset;
        onset.timeSeconds = refinedFrame * impl.hopSeconds;
        onset.frame = static_cast<FrameIndex>(std::llround(onset.timeSeconds * static_cast<double>(impl.sampleRate)));
        onset.strength      = p.strength;
        onset.bandMask      = p.frameIndex < impl.bandMasks.size() ? impl.bandMasks[p.frameIndex] : 0;
        m_result->onsets.push_back(onset);
    }

    // --- Tempo ---
    const auto tempoEstimate = estimateTempo(normalisedOdf, impl.hopSeconds, m_config.tempo);
    m_result->tempoConfidence = tempoEstimate.tempoConfidence;
    m_result->alternatives    = tempoEstimate.alternatives;

    // Refine the autocorrelation's inherently coarse (integer-lag, quantised-by-hop) estimate
    // using the onsets' own inter-onset intervals — onset times are already quadratic-refined to
    // sub-frame precision (peak_pick.cpp), which the ODF's hop size otherwise throws away. This is
    // what gets a click track from "a full BPM off" to the doc's ±0.1 BPM acceptance criterion.
    // Only intervals close to the autocorrelation's own estimate are used, so a handful of missed/
    // spurious onsets (or genuinely irregular material) can't drag the refinement off course —
    // falls back to the raw autocorrelation estimate whenever too few intervals qualify.
    double refinedBpm = tempoEstimate.primaryBpm;
    if (tempoEstimate.primaryBpm > 0.0 && m_result->onsets.size() >= 3) {
        const double        targetPeriod = 60.0 / tempoEstimate.primaryBpm;
        std::vector<double> intervals;
        for (std::size_t i = 1; i < m_result->onsets.size(); ++i) {
            const double interval = m_result->onsets[i].timeSeconds - m_result->onsets[i - 1].timeSeconds;
            if (interval > targetPeriod * 0.85 && interval < targetPeriod * 1.15) intervals.push_back(interval);
        }
        if (intervals.size() >= 3) {
            std::sort(intervals.begin(), intervals.end());
            const double medianInterval = intervals[intervals.size() / 2];
            if (medianInterval > 0.0) refinedBpm = 60.0 / medianInterval;
        }
    }
    m_result->primaryBpm = refinedBpm;

    const auto seriesResult = estimateTempoSeries(normalisedOdf, impl.hopSeconds, m_config.tempo);
    m_result->tempoSeries    = seriesResult.tempoSeries;
    m_result->tempoIsStable  = seriesResult.tempoIsStable;

    // --- Beats ---
    if (refinedBpm > 0.0) {
        const double periodSeconds = 60.0 / refinedBpm;
        const auto   trackResult    = trackBeats(normalisedOdf, impl.hopSeconds, periodSeconds, m_config.beatTracker);

        m_result->phaseConfidence = trackResult.phaseConfidence;
        m_result->beats.reserve(trackResult.beatFrames.size());
        for (std::size_t i = 0; i < trackResult.beatFrames.size(); ++i) {
            // The DP only resolves beats to whole ODF frames (~11.6ms at the default hop) — snap
            // to the nearest already sub-frame-refined onset when one exists close by, same idea
            // as the tempo refinement above, so beat times inherit onset precision rather than
            // being limited to the hop (needed for the doc's ±5ms click-track acceptance).
            const double approxTimeDp  = static_cast<double>(trackResult.beatFrames[i]) * impl.hopSeconds;
            const double snapTolerance = periodSeconds * 0.15;
            double       approxTime    = approxTimeDp;
            double       bestDelta     = snapTolerance;
            for (const Onset& onset : m_result->onsets) {
                const double delta = std::fabs(onset.timeSeconds - approxTimeDp);
                if (delta <= bestDelta) {
                    bestDelta  = delta;
                    approxTime = onset.timeSeconds;
                }
            }

            Beat beat;
            beat.timeSeconds    = approxTime;
            beat.frame           = static_cast<FrameIndex>(std::llround(approxTime * static_cast<double>(impl.sampleRate)));
            beat.confidence       = trackResult.beatStrengths[i];
            beat.beatIndexInBar = -1;  // v1: beats only, no automatic downbeat (doc's deferred decision)
            m_result->beats.push_back(beat);
        }
    }

    return AnalysisResult{};
}

std::unique_ptr<Analyzer> makeBeatAnalyzer(BeatResult& result, BeatConfig config) {
    return std::make_unique<BeatAnalyzer>(result, config);
}

BeatResult applyManualEdits(const BeatResult& detected, const BeatEdits& edits) {
    BeatResult out = detected;

    if (edits.tempoOverrideBpm.has_value() && *edits.tempoOverrideBpm > 0.0) {
        out.primaryBpm      = *edits.tempoOverrideBpm;
        out.tempoConfidence = 1.0f;  // a manual override is, by definition, certain
    }

    for (Beat& beat : out.beats) beat.timeSeconds += edits.phaseNudgeSeconds;

    if (!edits.removedBeatSeconds.empty()) {
        constexpr double kToleranceSeconds = 0.02;
        out.beats.erase(std::remove_if(out.beats.begin(), out.beats.end(),
                                          [&](const Beat& b) {
                                              return std::any_of(edits.removedBeatSeconds.begin(), edits.removedBeatSeconds.end(),
                                                                   [&](double t) { return std::fabs(t - b.timeSeconds) <= kToleranceSeconds; });
                                          }),
                          out.beats.end());
    }

    // sampleRate = hopSize / odfHopSeconds (both echoed in the detected result).
    const double sampleRate = detected.odfHopSeconds > 0.0
                                    ? static_cast<double>(detected.parametersUsed.hopSize) / detected.odfHopSeconds
                                    : 0.0;
    for (double t : edits.addedBeatSeconds) {
        Beat beat;
        beat.timeSeconds     = t;
        beat.frame            = static_cast<FrameIndex>(std::llround(t * sampleRate));
        beat.confidence        = 1.0f;  // manually added — treated as certain
        beat.beatIndexInBar   = -1;
        out.beats.push_back(beat);
    }

    std::sort(out.beats.begin(), out.beats.end(), [](const Beat& a, const Beat& b) { return a.timeSeconds < b.timeSeconds; });

    if (edits.downbeatTimeSeconds.has_value() && !out.beats.empty()) {
        std::size_t nearest = 0;
        double      bestDelta = std::fabs(out.beats[0].timeSeconds - *edits.downbeatTimeSeconds);
        for (std::size_t i = 1; i < out.beats.size(); ++i) {
            const double delta = std::fabs(out.beats[i].timeSeconds - *edits.downbeatTimeSeconds);
            if (delta < bestDelta) { bestDelta = delta; nearest = i; }
        }

        const int beatsPerBar = edits.timeSignatureBeatsPerBar > 0
                                     ? edits.timeSignatureBeatsPerBar
                                     : (detected.parametersUsed.timeSignatureBeatsPerBar > 0
                                            ? detected.parametersUsed.timeSignatureBeatsPerBar
                                            : 4);
        for (std::size_t i = 0; i < out.beats.size(); ++i) {
            const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>(i) - static_cast<std::ptrdiff_t>(nearest);
            std::ptrdiff_t indexInBar   = offset % beatsPerBar;
            if (indexInBar < 0) indexInBar += beatsPerBar;
            out.beats[i].beatIndexInBar = static_cast<std::int32_t>(indexInBar);
        }
    }

    return out;
}

std::string BeatResult::toJson() const {
    std::ostringstream out;
    out << "{\"schemaVersion\":\"1.0.0\",";
    out << "\"primaryBpm\":" << jsonNumber(primaryBpm) << ",";
    out << "\"tempoConfidence\":" << jsonNumber(tempoConfidence) << ",";
    out << "\"phaseConfidence\":" << jsonNumber(phaseConfidence) << ",";
    out << "\"tempoIsStable\":" << (tempoIsStable ? "true" : "false") << ",";
    out << "\"odfHopSeconds\":" << jsonNumber(odfHopSeconds) << ",";

    out << "\"alternatives\":[";
    for (std::size_t i = 0; i < alternatives.size(); ++i) {
        if (i > 0) out << ",";
        out << "{\"bpm\":" << jsonNumber(alternatives[i].bpm) << ",\"score\":" << jsonNumber(alternatives[i].score) << "}";
    }
    out << "],";

    out << "\"onsets\":[";
    for (std::size_t i = 0; i < onsets.size(); ++i) {
        if (i > 0) out << ",";
        const auto& o = onsets[i];
        out << "{\"timeSeconds\":" << jsonNumber(o.timeSeconds) << ",\"strength\":" << jsonNumber(o.strength)
            << ",\"bandMask\":" << static_cast<int>(o.bandMask) << "}";
    }
    out << "],";

    out << "\"beats\":[";
    for (std::size_t i = 0; i < beats.size(); ++i) {
        if (i > 0) out << ",";
        const auto& b = beats[i];
        out << "{\"timeSeconds\":" << jsonNumber(b.timeSeconds) << ",\"confidence\":" << jsonNumber(b.confidence)
            << ",\"beatIndexInBar\":" << b.beatIndexInBar << "}";
    }
    out << "]";

    out << "}";
    return out.str();
}

}  // namespace aud::beats

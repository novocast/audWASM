#include "true_peak.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../../util/assert.hpp"

namespace aud::loudness {

namespace {
constexpr double kSilenceDb = -std::numeric_limits<double>::infinity();

double linearToDb(double linear) noexcept {
    return linear > 0.0 ? 20.0 * std::log10(linear) : kSilenceDb;
}
}  // namespace

TruePeakMeter::TruePeakMeter(SampleRate sourceRate, ChannelIndex channels, std::uint32_t oversampling)
    : m_channels(channels),
      m_factor(oversampling),
      m_resampler(sourceRate, sourceRate * oversampling, channels, playback::Resampler::Quality::Best) {
    AUD_ASSERT(oversampling == 4 || oversampling == 8 || oversampling == 16,
               "true peak oversampling must be 4, 8 or 16 per BS.1770-4 Annex 2");

    m_truePeakLinear.assign(channels, 0.0);
    m_samplePeakLinear.assign(channels, 0.0);
    m_scratch.resize(channels);
    m_scratchSpans.resize(channels);
}

void TruePeakMeter::enableIspEventCapture(double thresholdDbtp, std::size_t maxEvents) noexcept {
    m_ispCaptureEnabled  = true;
    m_ispThresholdLinear = std::pow(10.0, thresholdDbtp / 20.0);
    m_ispMaxEvents       = maxEvents;
}

void TruePeakMeter::process(std::span<const std::span<const Sample>> planarChannels) {
    if (planarChannels.empty()) return;
    const std::size_t framesIn = planarChannels[0].size();

    // Unweighted sample peak falls out of the same scan, no oversampling needed for it. Also
    // tracked per-call (m_chunkSamplePeakLinear, below) so an ISP event found in this same call can
    // report the plain sample peak in its neighbourhood alongside the oversampled true peak (M11:
    // "the sample peak in the same neighbourhood, so the user can see 'samples read -0.3 dBFS but
    // the true peak is +0.8 dBTP'") — coarse (whole-chunk, not a tight window) but cheap and free
    // of a second pass.
    std::vector<double> chunkSamplePeakLinear(m_channels, 0.0);
    for (ChannelIndex ch = 0; ch < m_channels && ch < planarChannels.size(); ++ch) {
        double peak = m_samplePeakLinear[ch];
        double chunkPeak = 0.0;
        for (Sample s : planarChannels[ch]) {
            const double mag = static_cast<double>(std::fabs(s));
            peak      = std::max(peak, mag);
            chunkPeak = std::max(chunkPeak, mag);
        }
        m_samplePeakLinear[ch] = peak;
        chunkSamplePeakLinear[ch] = chunkPeak;
    }
    m_chunkSamplePeakLinear = chunkSamplePeakLinear;

    // Capacity generous enough that a single call almost always drains everything the lookahead
    // allows; any remainder (rare — only when the very first chunk is shorter than the filter's
    // half-span) is caught by the zero-input drain loop below.
    const std::size_t capacity = framesIn * m_factor + 256;
    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        m_scratch[ch].assign(capacity, 0.0f);
        m_scratchSpans[ch] = std::span<Sample>(m_scratch[ch]);
    }

    std::size_t produced = m_resampler.process(planarChannels, framesIn, m_scratchSpans, capacity);
    scanOversampledOutput(m_scratchSpans, produced);

    // Drain any backlog the single call above couldn't fit (should be rare given the generous
    // capacity) by re-invoking with zero new input until nothing more comes out.
    std::vector<std::span<const Sample>> emptyIn(m_channels, std::span<const Sample>{});
    while (produced > 0) {
        produced = m_resampler.process(emptyIn, 0, m_scratchSpans, capacity);
        if (produced > 0) scanOversampledOutput(m_scratchSpans, produced);
    }
}

void TruePeakMeter::finish() {
    const std::size_t capacity = 4096;
    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        m_scratch[ch].assign(capacity, 0.0f);
        m_scratchSpans[ch] = std::span<Sample>(m_scratch[ch]);
    }
    std::size_t produced = m_resampler.drain(m_scratchSpans, capacity);
    while (produced > 0) {
        scanOversampledOutput(m_scratchSpans, produced);
        produced = m_resampler.drain(m_scratchSpans, capacity);
    }
}

void TruePeakMeter::scanOversampledOutput(std::span<const std::span<Sample>> out, std::size_t framesProduced) {
    for (std::size_t i = 0; i < framesProduced; ++i) {
        for (ChannelIndex ch = 0; ch < m_channels && ch < out.size(); ++ch) {
            const double magnitude = std::fabs(static_cast<double>(out[ch][i]));
            if (magnitude > m_truePeakLinear[ch]) {
                m_truePeakLinear[ch] = magnitude;
            }
            if (magnitude > m_overallPeakLinear) {
                m_overallPeakLinear = magnitude;
                // Inverse of the resampler's step: source frame index for output frame N is
                // N / factor (step = 1/factor exactly, by construction — see TruePeakMeter ctor).
                m_peakFrame = static_cast<FrameIndex>(
                    std::llround((m_outputFrameCount + static_cast<double>(i)) / static_cast<double>(m_factor)));
            }

            // M11: record *every* excursion above the configured threshold, not just the running
            // max — the marginal cost over the max-tracking above is this one extra comparison and
            // an occasional push_back.
            if (m_ispCaptureEnabled && magnitude > m_ispThresholdLinear) {
                ++m_ispEventCountTotal;
                if (m_ispMaxEvents == 0 || m_ispEvents.size() < m_ispMaxEvents) {
                    const FrameIndex sourceFrame = static_cast<FrameIndex>(std::llround(
                        (m_outputFrameCount + static_cast<double>(i)) / static_cast<double>(m_factor)));
                    const double samplePeakNearby = ch < m_chunkSamplePeakLinear.size()
                                                          ? m_chunkSamplePeakLinear[ch]
                                                          : magnitude;
                    m_ispEvents.push_back(IspEvent{
                        sourceFrame,
                        ch,
                        linearToDb(magnitude),
                        linearToDb(samplePeakNearby),
                    });
                }
            }
        }
    }
    m_outputFrameCount += static_cast<double>(framesProduced);
}

double TruePeakMeter::truePeakDbtpFor(ChannelIndex channel) const noexcept {
    return channel < m_truePeakLinear.size() ? linearToDb(m_truePeakLinear[channel]) : kSilenceDb;
}

double TruePeakMeter::samplePeakDbfsFor(ChannelIndex channel) const noexcept {
    return channel < m_samplePeakLinear.size() ? linearToDb(m_samplePeakLinear[channel]) : kSilenceDb;
}

double TruePeakMeter::truePeakDbtpOverall() const noexcept { return linearToDb(m_overallPeakLinear); }

}  // namespace aud::loudness

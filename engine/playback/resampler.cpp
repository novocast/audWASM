#include "resampler.hpp"

#include <algorithm>
#include <cmath>

#include "../util/assert.hpp"

namespace aud::playback {

namespace {

constexpr double kPi = 3.14159265358979323846;

double sinc(double x) noexcept {
    if (std::abs(x) < 1e-9) {
        return 1.0;
    }
    const double px = kPi * x;
    return std::sin(px) / px;
}

// Zeroth-order modified Bessel function of the first kind, via its power series. Converges quickly
// for the beta range Kaiser windows use in practice (beta < ~12); 32 terms is comfortably enough.
double besselI0(double x) noexcept {
    double sum  = 1.0;
    double term = 1.0;
    const double xHalfSq = (x * 0.5) * (x * 0.5);
    for (int k = 1; k < 32; ++k) {
        term *= xHalfSq / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-15 * sum) {
            break;
        }
    }
    return sum;
}

double kaiserBeta(double stopbandAttenuationDb) noexcept {
    const double a = stopbandAttenuationDb;
    if (a > 50.0) {
        return 0.1102 * (a - 8.7);
    }
    if (a >= 21.0) {
        return 0.5842 * std::pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);
    }
    return 0.0;
}

struct QualityParams {
    std::size_t numTaps;
    std::size_t phaseCount;
    double      stopbandAttenuationDb;
};

QualityParams paramsFor(Resampler::Quality quality) noexcept {
    switch (quality) {
        case Resampler::Quality::Fast: return {16, 64, 60.0};
        case Resampler::Quality::Best: return {64, 256, 100.0};
        case Resampler::Quality::Good:
        default:                       return {32, 128, 80.0};
    }
}

}  // namespace

Resampler::Resampler(SampleRate sourceRate, SampleRate targetRate, ChannelIndex channels, Quality quality)
    : m_sourceRate(sourceRate), m_targetRate(targetRate), m_channels(channels) {
    AUD_ASSERT(sourceRate > 0 && targetRate > 0, "Resampler requires non-zero sample rates");
    AUD_ASSERT(channels > 0, "Resampler requires at least one channel");

    m_history.resize(channels);

    if (isIdentity()) {
        return;  // no filter needed; process() takes the bypass path
    }

    const QualityParams params = paramsFor(quality);
    m_numTaps    = params.numTaps;
    m_phaseCount = params.phaseCount;
    m_step       = static_cast<double>(sourceRate) / static_cast<double>(targetRate);
    m_cutoff     = 0.5 * std::min(1.0, static_cast<double>(targetRate) / static_cast<double>(sourceRate));

    const double halfSpan = static_cast<double>(m_numTaps) / 2.0;
    const double beta     = kaiserBeta(params.stopbandAttenuationDb);
    const double i0Beta   = besselI0(beta);
    const auto   halfTaps = static_cast<double>(m_numTaps / 2);

    // phaseCount + 1 rows: convolve() linearly interpolates between adjacent rows using the
    // sub-phase remainder, rather than snapping to the nearest of phaseCount discrete phases. The
    // extra row (p == phaseCount, phaseFrac == 1.0) is a perfectly well-defined evaluation of the
    // same continuous kernel — nothing wraps here, it's just the next point on the curve.
    m_table.assign((m_phaseCount + 1) * m_numTaps, 0.0f);
    for (std::size_t p = 0; p <= m_phaseCount; ++p) {
        const double phaseFrac = static_cast<double>(p) / static_cast<double>(m_phaseCount);
        double       rowSum     = 0.0;
        for (std::size_t k = 0; k < m_numTaps; ++k) {
            const double u = (static_cast<double>(k) - halfTaps + 1.0) - phaseFrac;
            double       w = 0.0;
            if (std::abs(u) < halfSpan) {
                const double radicand = std::max(0.0, 1.0 - (u / halfSpan) * (u / halfSpan));
                w                     = besselI0(beta * std::sqrt(radicand)) / i0Beta;
            }
            const double h                       = 2.0 * m_cutoff * sinc(2.0 * m_cutoff * u) * w;
            m_table[p * m_numTaps + k]            = static_cast<float>(h);
            rowSum += h;
        }
        // Force exact unity DC gain per phase — the window slightly perturbs the ideal filter's
        // theoretical unity gain, and any resulting level error would show up as an audible (and
        // measurable, per the M03 THD+N acceptance criterion) amplitude ripple across phases.
        if (rowSum != 0.0) {
            const float inv = static_cast<float>(1.0 / rowSum);
            for (std::size_t k = 0; k < m_numTaps; ++k) {
                m_table[p * m_numTaps + k] *= inv;
            }
        }
    }
}

Sample Resampler::convolve(const ChannelHistory& history, double sourcePos) const noexcept {
    const auto   floorPos = static_cast<std::int64_t>(sourcePos);
    const double frac     = sourcePos - static_cast<double>(floorPos);

    // Linear interpolation between the two nearest of the phaseCount+1 precomputed phase rows.
    // Snapping to the single nearest phase (no interpolation) quantises the fractional delay to
    // steps of 1/phaseCount of a sample, which for phaseCount=128 measures out to roughly -66dB of
    // spurious distortion — short of the -80dB THD+N this engine targets. Interpolating between
    // rows removes nearly all of that error at the cost of one extra multiply-add per tap.
    const double       phasePos    = frac * static_cast<double>(m_phaseCount);
    const auto         phaseIdx0   = static_cast<std::size_t>(phasePos);
    const double       phaseSubFrac = phasePos - static_cast<double>(phaseIdx0);
    const std::size_t  phaseIdx1   = std::min(phaseIdx0 + 1, m_phaseCount);

    const auto halfTaps = static_cast<std::int64_t>(m_numTaps / 2);
    const std::int64_t idx0 = floorPos - halfTaps + 1;

    double acc = 0.0;
    for (std::size_t k = 0; k < m_numTaps; ++k) {
        const std::int64_t absIdx    = idx0 + static_cast<std::int64_t>(k);
        const std::int64_t historyRel = absIdx - static_cast<std::int64_t>(m_historyBase);
        float               sample    = 0.0f;
        if (historyRel >= 0 && static_cast<std::size_t>(historyRel) < history.samples.size()) {
            sample = history.samples[static_cast<std::size_t>(historyRel)];
        }
        const double coeff0 = static_cast<double>(m_table[phaseIdx0 * m_numTaps + k]);
        const double coeff1 = static_cast<double>(m_table[phaseIdx1 * m_numTaps + k]);
        const double coeff  = coeff0 + (coeff1 - coeff0) * phaseSubFrac;
        acc += static_cast<double>(sample) * coeff;
    }
    return static_cast<Sample>(acc);
}

void Resampler::compactHistory() {
    const auto halfTaps = static_cast<std::int64_t>(m_numTaps / 2);
    const auto floorPos = static_cast<std::int64_t>(m_sourcePos);
    const std::int64_t keepFromAbsSigned = floorPos - halfTaps;
    const std::size_t  keepFromAbs = keepFromAbsSigned > 0 ? static_cast<std::size_t>(keepFromAbsSigned) : 0;

    if (keepFromAbs <= m_historyBase) {
        return;
    }
    const std::size_t drop = keepFromAbs - m_historyBase;
    for (auto& channel : m_history) {
        if (drop >= channel.samples.size()) {
            channel.samples.clear();
        } else {
            channel.samples.erase(channel.samples.begin(), channel.samples.begin() + static_cast<std::ptrdiff_t>(drop));
        }
    }
    m_historyBase = keepFromAbs;
}

std::size_t Resampler::process(std::span<const std::span<const Sample>> planarIn, std::size_t framesIn,
                                std::span<std::span<Sample>> planarOut, std::size_t maxFramesOut) {
    if (isIdentity()) {
        const std::size_t n = std::min(framesIn, maxFramesOut);
        for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
            std::copy_n(planarIn[ch].data(), n, planarOut[ch].data());
        }
        m_sourcePos += static_cast<double>(n);
        m_totalFed += framesIn;
        return n;
    }

    for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
        auto& dst = m_history[ch].samples;
        dst.insert(dst.end(), planarIn[ch].data(), planarIn[ch].data() + framesIn);
    }
    m_totalFed += framesIn;

    const std::size_t halfTaps = m_numTaps / 2;
    std::size_t       produced = 0;
    while (produced < maxFramesOut) {
        const auto floorPos = static_cast<std::size_t>(m_sourcePos);
        if (floorPos + halfTaps >= m_totalFed) {
            break;  // not enough lookahead yet — need more input
        }
        for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
            planarOut[ch][produced] = convolve(m_history[ch], m_sourcePos);
        }
        m_sourcePos += m_step;
        ++produced;
    }

    compactHistory();
    return produced;
}

std::size_t Resampler::drain(std::span<std::span<Sample>> planarOut, std::size_t maxFramesOut) {
    if (isIdentity()) {
        return 0;
    }

    const std::size_t halfTaps = m_numTaps / 2;
    std::size_t       produced = 0;
    while (produced < maxFramesOut) {
        const auto floorPos = static_cast<std::size_t>(m_sourcePos);
        if (floorPos >= m_totalFed + halfTaps) {
            break;  // fully past the kernel's support — nothing meaningful left to emit
        }
        for (ChannelIndex ch = 0; ch < m_channels; ++ch) {
            planarOut[ch][produced] = convolve(m_history[ch], m_sourcePos);
        }
        m_sourcePos += m_step;
        ++produced;
    }
    return produced;
}

void Resampler::reset() noexcept {
    for (auto& channel : m_history) {
        channel.samples.clear();
    }
    m_historyBase = 0;
    m_totalFed    = 0;
    m_sourcePos   = 0.0;
}

}  // namespace aud::playback

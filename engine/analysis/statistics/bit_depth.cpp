#include "bit_depth.hpp"

#include <bit>
#include <cmath>
#include <cstdio>

namespace aud::statistics {

namespace {

// Trailing zero count of a 64-bit value; 64 for zero (meaning "no information").
std::uint32_t trailingZeros(std::uint64_t v) {
    return v == 0 ? 64u : static_cast<std::uint32_t>(std::countr_zero(v));
}

}  // namespace

void BitDepthAccumulator::begin(std::uint32_t containerBitDepth) noexcept {
    m_containerBitDepth = containerBitDepth;
    m_intScale           = containerBitDepth == 0 ? 0.0 : static_cast<double>(1ull << (containerBitDepth - 1));

    m_orAccumulator  = 0;
    m_nonZeroSamples = 0;
    m_totalSamples   = 0;
    m_hasPrevInt     = false;
    m_prevInt        = 0;
    m_lsbToggles     = 0;
    m_transitions    = 0;
    m_maxError.fill(0.0);
}

void BitDepthAccumulator::process(std::span<const Sample> samples) noexcept {
    m_totalSamples += samples.size();

    if (m_containerBitDepth == 0) {
        // Float source: track how well each candidate integer grid round-trips.
        for (std::size_t c = 0; c < kFloatQuantisationCandidates.size(); ++c) {
            const double scale = static_cast<double>(1ull << (kFloatQuantisationCandidates[c] - 1));
            double       err   = m_maxError[c];
            for (Sample s : samples) {
                const double v      = static_cast<double>(s);
                const double quant  = std::round(v * scale) / scale;
                const double delta  = std::fabs(v - quant);
                if (delta > err) err = delta;
            }
            m_maxError[c] = err;
        }
        return;
    }

    for (Sample s : samples) {
        const double       v      = static_cast<double>(s);
        const std::int64_t intVal = static_cast<std::int64_t>(std::llround(v * m_intScale));

        if (intVal != 0) {
            m_orAccumulator |= static_cast<std::uint64_t>(intVal);
            ++m_nonZeroSamples;
        }

        if (m_hasPrevInt) {
            ++m_transitions;
            if ((m_prevInt & 1) != (intVal & 1)) ++m_lsbToggles;
        }
        m_prevInt    = intVal;
        m_hasPrevInt = true;
    }
}

BitDepthResult BitDepthAccumulator::finish() const {
    BitDepthResult result;
    result.containerBitDepth = m_containerBitDepth;

    if (m_containerBitDepth == 0) {
        // Smallest candidate depth whose round-trip error is negligible (well under one code step
        // at that depth) wins; none qualifying means "no quantisation detected".
        for (std::size_t c = 0; c < kFloatQuantisationCandidates.size(); ++c) {
            const std::uint32_t depth = kFloatQuantisationCandidates[c];
            const double        step  = 1.0 / static_cast<double>(1ull << (depth - 1));
            if (m_maxError[c] < step * 0.01) {
                result.effectiveBitDepth = depth;
                break;
            }
        }
        // Dither heuristic doesn't apply to float sources with no detected grid.
        return result;
    }

    if (m_nonZeroSamples == 0) {
        // Pure silence: no information to derive an effective depth from.
        result.effectiveBitDepth = m_containerBitDepth;
        return result;
    }

    const std::uint32_t k = trailingZeros(m_orAccumulator);
    const std::uint32_t effective =
        k >= m_containerBitDepth ? m_containerBitDepth : m_containerBitDepth - k;
    result.effectiveBitDepth = effective;

    // Dither heuristic: only interesting when every bit down to the LSB is populated (k == 0) —
    // otherwise there are unused low bits and no dither question to ask. A dithered LSB toggles
    // close to 50% of the time, uncorrelated with the signal; hedge hard, never assert.
    if (k == 0 && m_transitions > 0) {
        const double toggleRate = static_cast<double>(m_lsbToggles) / static_cast<double>(m_transitions);
        const double distanceFromRandom = std::fabs(toggleRate - 0.5) * 2.0;  // 0 at random, 1 at never/always
        if (distanceFromRandom < 0.3) {
            result.ditherLikely     = true;
            result.ditherConfidence = 1.0 - distanceFromRandom / 0.3;
        }
    }

    return result;
}

std::string BitDepthResult::describe() const {
    char buf[128];
    if (containerBitDepth == 0) {
        if (!effectiveBitDepth.has_value()) {
            return "float, no quantisation detected";
        }
        std::snprintf(buf, sizeof(buf), "float, %u-bit quantisation grid detected", *effectiveBitDepth);
        return buf;
    }
    const std::uint32_t effective = effectiveBitDepth.value_or(containerBitDepth);
    std::snprintf(buf, sizeof(buf), "%u-bit container, %u-bit effective content", containerBitDepth, effective);
    return buf;
}

BitDepthResult detectEffectiveBitDepth(std::span<const Sample> samples, std::uint32_t containerBitDepth) {
    BitDepthAccumulator acc;
    acc.begin(containerBitDepth);
    acc.process(samples);
    return acc.finish();
}

}  // namespace aud::statistics

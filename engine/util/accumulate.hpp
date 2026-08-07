#pragma once

// Pairwise summation helpers shared by the waveform reducer (M04) and the statistics analyser
// (M09). Naive left-to-right accumulation over a 65536-frame chunk loses meaningful precision on
// quiet material (float has ~7 decimal digits); pairwise summation keeps the running error at
// O(log n) ulps instead of O(n) for close to free, by halving the range recursively down to a
// small base case that sums naively.

#include <cstddef>
#include <span>

#include "audio_types.hpp"

namespace aud {

namespace detail {
inline constexpr std::size_t kPairwiseBaseCase = 128;
}  // namespace detail

// sum(samples[i]) via pairwise summation.
template <class T = double>
[[nodiscard]] T pairwiseSum(std::span<const Sample> samples) noexcept {
    const std::size_t n = samples.size();
    if (n == 0) {
        return T{0};
    }
    if (n <= detail::kPairwiseBaseCase) {
        T sum{0};
        for (Sample s : samples) {
            sum += static_cast<T>(s);
        }
        return sum;
    }
    const std::size_t half = n / 2;
    return pairwiseSum<T>(samples.first(half)) + pairwiseSum<T>(samples.subspan(half));
}

// sum(samples[i]^2) via pairwise summation — the workhorse for RMS (M04) and any sum-of-squares
// statistic (M09).
template <class T = double>
[[nodiscard]] T pairwiseSumSquares(std::span<const Sample> samples) noexcept {
    const std::size_t n = samples.size();
    if (n == 0) {
        return T{0};
    }
    if (n <= detail::kPairwiseBaseCase) {
        T sum{0};
        for (Sample s : samples) {
            const T v = static_cast<T>(s);
            sum += v * v;
        }
        return sum;
    }
    const std::size_t half = n / 2;
    return pairwiseSumSquares<T>(samples.first(half)) + pairwiseSumSquares<T>(samples.subspan(half));
}

// sum(|samples[i]|) via pairwise summation — M09's mean-absolute-deviation-style statistics.
template <class T = double>
[[nodiscard]] T pairwiseSumAbs(std::span<const Sample> samples) noexcept {
    const std::size_t n = samples.size();
    if (n == 0) {
        return T{0};
    }
    if (n <= detail::kPairwiseBaseCase) {
        T sum{0};
        for (Sample s : samples) {
            sum += static_cast<T>(s < 0 ? -s : s);
        }
        return sum;
    }
    const std::size_t half = n / 2;
    return pairwiseSumAbs<T>(samples.first(half)) + pairwiseSumAbs<T>(samples.subspan(half));
}

// sum((samples[i] - shift) * (samples[i] - shift)) via pairwise summation — M09's variance
// accumulation uses this with `shift` set to an arbitrary near-the-data constant (the first
// sample seen), which is the "shifted sum of squares" trick: Var(x) = Var(x - shift) for any
// constant shift, and (x - shift) has no DC component to catastrophically cancel against, unlike
// naive E[x^2] - E[x]^2 on data with a large DC offset relative to its AC swing.
template <class T = double>
[[nodiscard]] T pairwiseSumSquaresShifted(std::span<const Sample> samples, T shift) noexcept {
    const std::size_t n = samples.size();
    if (n == 0) {
        return T{0};
    }
    if (n <= detail::kPairwiseBaseCase) {
        T sum{0};
        for (Sample s : samples) {
            const T v = static_cast<T>(s) - shift;
            sum += v * v;
        }
        return sum;
    }
    const std::size_t half = n / 2;
    return pairwiseSumSquaresShifted<T>(samples.first(half), shift) +
           pairwiseSumSquaresShifted<T>(samples.subspan(half), shift);
}

}  // namespace aud

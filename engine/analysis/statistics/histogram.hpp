#pragma once

// 1024-bucket amplitude histogram, symmetric around zero. Feeds M09's "distribution plot" and is
// the raw material the bit-depth/dither heuristics in bit_depth.hpp reason about separately —
// this header only ever counts, it never interprets.

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

#include "../../util/audio_types.hpp"

namespace aud::statistics {

inline constexpr std::uint32_t kHistogramBuckets = 1024;

// Fixed [-1, 1] linear range regardless of the file's actual peak — the histogram's job is to show
// where the material sits relative to full scale, not to auto-zoom on quiet content (that's the
// windowed RMS series' job). Samples beyond +-1 (headroom) clamp into the outermost bucket.
class AmplitudeHistogram {
public:
    // Bucket `b` covers [lo(b), hi(b)); bucket kHistogramBuckets/2 is the one straddling zero.
    void add(Sample value) noexcept {
        ++m_buckets[bucketFor(value)];
    }

    void addRange(std::span<const Sample> values) noexcept {
        for (Sample v : values) add(v);
    }

    void merge(const AmplitudeHistogram& other) noexcept {
        for (std::uint32_t i = 0; i < kHistogramBuckets; ++i) {
            m_buckets[i] += other.m_buckets[i];
        }
    }

    [[nodiscard]] const std::array<std::uint32_t, kHistogramBuckets>& buckets() const noexcept {
        return m_buckets;
    }

    // Centre value of bucket `b`, for plotting.
    [[nodiscard]] static double bucketCentre(std::uint32_t b) noexcept {
        constexpr double kWidth = 2.0 / static_cast<double>(kHistogramBuckets);
        return -1.0 + (static_cast<double>(b) + 0.5) * kWidth;
    }

    // log1p-compressed count, purely a display convenience (task: "log option for display") — the
    // stored counts themselves stay linear so re-deriving this never loses information.
    [[nodiscard]] static double logDisplay(std::uint32_t count) noexcept {
        return std::log1p(static_cast<double>(count));
    }

private:
    [[nodiscard]] static std::uint32_t bucketFor(Sample value) noexcept {
        constexpr double kWidth = 2.0 / static_cast<double>(kHistogramBuckets);
        double            v     = static_cast<double>(value);
        if (v < -1.0) v = -1.0;
        if (v > 1.0) v = 1.0 - 1e-12;  // keep +1.0 out of the (nonexistent) 1025th bucket
        auto bucket = static_cast<std::int64_t>((v + 1.0) / kWidth);
        if (bucket < 0) bucket = 0;
        if (bucket >= static_cast<std::int64_t>(kHistogramBuckets)) bucket = kHistogramBuckets - 1;
        return static_cast<std::uint32_t>(bucket);
    }

    std::array<std::uint32_t, kHistogramBuckets> m_buckets{};
};

}  // namespace aud::statistics

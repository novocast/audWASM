#pragma once

// M13's adaptive spectral whitening ("Adaptive whitening and normalisation"): per-bin decaying
// maximum, magnitude divided by it. Applied before ODF computation so quiet high-frequency onsets
// (which a fixed threshold would miss on any track with real dynamics) become visible without
// amplifying noise floor in bins that never carried signal.

#include <cstddef>
#include <span>
#include <vector>

namespace aud::beats {

struct WhiteningConfig {
    // Per-frame decay of the tracked per-bin peak. At the default 2048/512 hop (~11.6ms/frame),
    // 0.9997 gives a ~35-frame (~400ms) half-life — fast enough to track a track's dynamics,
    // slow enough not to whiten away a single onset's own transient before flux sees it.
    double decayPerFrame = 0.9997;
    float  floorLinear   = 1e-6f;  // avoids dividing by ~0 in silence
};

// Streaming, in-place: one instance per channel/mix, fed frames in order.
class SpectralWhitener {
public:
    explicit SpectralWhitener(std::size_t binCount, WhiteningConfig config = {});

    // Divides each bin of `magnitudes` by its tracked decaying peak, in place.
    void apply(std::span<float> magnitudes) noexcept;

private:
    std::vector<float> m_peak;
    WhiteningConfig     m_config;
};

}  // namespace aud::beats

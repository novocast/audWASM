#include "whitening.hpp"

#include <algorithm>

namespace aud::beats {

SpectralWhitener::SpectralWhitener(std::size_t binCount, WhiteningConfig config)
    : m_peak(binCount, 0.0f), m_config(config) {}

void SpectralWhitener::apply(std::span<float> magnitudes) noexcept {
    const std::size_t n = std::min(magnitudes.size(), m_peak.size());
    for (std::size_t k = 0; k < n; ++k) {
        m_peak[k]       = std::max(magnitudes[k], static_cast<float>(m_config.decayPerFrame) * m_peak[k]);
        const float peak = std::max(m_peak[k], m_config.floorLinear);
        magnitudes[k]    = magnitudes[k] / peak;
    }
}

}  // namespace aud::beats

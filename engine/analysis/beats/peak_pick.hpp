#pragma once

// M13's peak picking: the standard three-condition rule over the normalised ODF (doc's "Peak
// picking" section) plus quadratic sub-frame refinement using the same technique as M06's
// fft/peak_interp.hpp (adapted: the ODF is already log-compressed and normalised upstream, so no
// further log() is applied here — just the quadratic fit through the peak and its two neighbours).

#include <cstddef>
#include <span>
#include <vector>

namespace aud::beats {

struct PeakPickConfig {
    std::size_t localMaxHalfWindow = 3;    // w frames each side (~35ms at the default hop)
    std::size_t meanHalfWindow     = 10;   // mw frames each side
    float       delta              = 0.1f;  // required margin above the local mean
    double      minInterOnsetSeconds = 0.03;  // refractory period
};

struct PickedPeak {
    std::size_t frameIndex;     // nearest ODF frame
    double      subFrameOffset;  // fractional refinement, in frames, in [-0.5, 0.5]
    float       strength;        // interpolated ODF value at the refined peak
};

// `odf` should be the normalised series (normalise.hpp), not the raw combined ODF — the three
// conditions assume a roughly zero-centred, unit-scale signal.
[[nodiscard]] std::vector<PickedPeak> pickPeaks(std::span<const float> odf, double hopSeconds,
                                                  PeakPickConfig config = {});

}  // namespace aud::beats

#pragma once

// M13's onset detection functions (ODFs). See documentation/tasks/M13-beat-detection.md
// "Onset detection functions": three ODFs computed per STFT frame and combined, rather than
// betting on one — spectral flux is strong on percussive/broadband onsets, complex-domain on
// tonal onsets, HFC on transients. Each is also reported band-limited (low/mid/high) because the
// low-band flux alone is an excellent kick detector and directly feeds M14.
//
// Flux is computed on log-compressed magnitude (`log(1 + lambda*|X|)`, per the doc's "Decision —
// compute flux on log-magnitude") rather than raw magnitude — this is the standard fix for flux
// being dominated by the loudest bins on wide-dynamics material.
//
// Combination decision: this class deliberately returns each raw, *unnormalised* component and
// combination happens elsewhere (BeatAnalyzer::finish(), via normalise.hpp) over each component's
// *whole* retained series, not online per-frame. An earlier version scaled each component online by
// its own decaying peak/EMA z-score — both approaches carry state forward from one occurrence to
// the next, and on a periodic source (a click track) that per-occurrence history measurably shifted
// where each onset's sub-frame peak landed (a systematic few-ms bias that grew clearly worse than
// an isolated, one-off transient). Normalising each component's full series against its own
// (history-independent, centred) median/MAD once the whole track is known removes that: every
// occurrence of the same transient is then treated identically. See normalise.hpp/cpp.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../util/audio_types.hpp"

namespace aud::beats {

struct OdfConfig {
    float fluxWeight    = 1.0f;
    float complexWeight = 1.0f;
    float hfcWeight     = 1.0f;

    // `log(1 + lambda*|X|)` compression applied before flux is computed.
    double logCompressionLambda = 1000.0;

    // Band boundaries for the band-limited flux variants (Hz).
    double lowBandHz  = 200.0;
    double highBandHz = 4000.0;
};

struct OdfSample {
    float flux          = 0.0f;  // raw (pre-normalisation) log-flux
    float complexDomain = 0.0f;  // raw complex-domain deviation
    float hfc           = 0.0f;  // raw high-frequency content

    float lowBandFlux  = 0.0f;
    float midBandFlux  = 0.0f;
    float highBandFlux = 0.0f;

    std::uint8_t bandMask = 0;  // bit0=low bit1=mid bit2=high — which bands this frame's onset energy favours
};

// Incremental ODF computation over an in-order stream of STFT frames (M06's StftProcessor). One
// instance owns exactly the previous frame's spectrum, nothing more — O(binCount) state.
class OdfComputer {
public:
    OdfComputer(std::size_t binCount, SampleRate sampleRate, OdfConfig config = {});

    // `magnitudes` and `complexBins` must both have length binCount() and correspond to the same
    // STFT frame (StftFrame::bins / StftFrame::complexBins). Frames must be fed in order.
    [[nodiscard]] OdfSample push(std::span<const float> magnitudes, std::span<const std::complex<float>> complexBins);

    [[nodiscard]] std::size_t binCount() const noexcept { return m_binCount; }

private:
    OdfConfig    m_config;
    std::size_t  m_binCount;
    // Only needed at construction (to derive m_lowBandBin/m_highBandBin below); retained rather than
    // dropped since it documents what those bin indices are relative to.
    [[maybe_unused]] SampleRate m_sampleRate;
    std::size_t  m_lowBandBin;
    std::size_t  m_highBandBin;

    std::vector<float> m_prevLogMag;
    std::vector<float> m_prevPhase;
    std::vector<float> m_prevPrevPhase;
    bool m_hasPrev     = false;
    bool m_hasPrevPrev = false;
};

}  // namespace aud::beats

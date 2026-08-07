#pragma once

// Streaming STFT processor (M06). Owns one RealFft plan for its lifetime (see real_fft.hpp) and
// feeds frames to a callback as they complete — an STFT of a whole track is never materialised
// (a 5-minute track at 2048/512/44.1kHz would be ~106MB per channel of magnitudes alone; see the
// M06 design doc's "Output layout & memory"). Consumers that need a reduction (onset detection,
// loudness, key) keep only their reduction; consumers that need random access (the spectrogram,
// M07) build a tile cache on top of this, they don't ask this class to hold everything.
//
// Decision — centred = true by default: frame t is centred on sample t*hop, with the signal
// conceptually zero-padded by fftSize/2 at both ends (matching librosa/scipy). Anything that
// reports a *time* (onset detection M13, transient detection M14) would otherwise be
// systematically late by fftSize/2 samples (23ms at 2048/44.1kHz — audible) unless every consumer
// remembered to compensate; fixing it once here is far safer.
//
// Streaming state: an internal buffer holds windowSize - hopSize samples of overlap across
// process() calls — STFT frames genuinely straddle chunk boundaries, unlike the waveform
// reduction, so this state is unavoidable (requiredLookbackFrames() exposes it for M00's
// parallel-by-block-of-frames scheme).

#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "../util/audio_types.hpp"
#include "../util/result.hpp"
#include "real_fft.hpp"
#include "scaling.hpp"
#include "simd_kernels.hpp"
#include "windows.hpp"

namespace aud::fft {

struct StftConfig {
    std::size_t     fftSize    = 2048;
    std::size_t     hopSize    = 512;              // 75% overlap at the defaults
    WindowType      window     = WindowType::Hann;
    bool            zeroPadded = false;             // analysis window < fftSize, zero-padded for interpolation
    std::size_t     windowSize = 0;                 // 0 => == fftSize
    SpectrumScaling scaling    = SpectrumScaling::Amplitude;
    bool            centered   = true;              // frame t centred on sample t*hop
};

// One analysis frame handed to the FrameCallback. `bins` (fftSize/2+1 magnitudes, already scaled
// per StftConfig::scaling) points at internal storage valid only for the duration of the callback.
struct StftFrame {
    std::size_t            frameIndex = 0;
    std::span<const float> bins;
    // Raw complex spectrum (unscaled, pre-magnitude), same length as `bins`. Added for M13's
    // complex-domain onset detection function, which needs phase — magnitude alone can't predict
    // it. Reassignment offsets (M06 "Reassignment & interpolation") are still future work and slot
    // in here without breaking this layout.
    std::span<const std::complex<float>> complexBins;
};

using FrameCallback = std::function<void(const StftFrame&)>;

class StftProcessor {
public:
    static Result<StftProcessor> create(const StftConfig& config, SampleRate sampleRate);

    StftProcessor(StftProcessor&&) noexcept            = default;
    StftProcessor& operator=(StftProcessor&&) noexcept = default;
    StftProcessor(const StftProcessor&)                = delete;
    StftProcessor& operator=(const StftProcessor&)     = delete;

    // Number of frames a stream of `totalFrames` samples would produce, without processing it —
    // matches what feeding that many samples through process()+finish() actually emits.
    [[nodiscard]] std::size_t frameCount(FrameIndex totalFrames) const noexcept;

    // The time, in seconds, that frame `frameIndex`'s analysis window is centred on.
    [[nodiscard]] double frameTimeSeconds(std::size_t frameIndex) const noexcept;

    // The centre frequency, in Hz, of FFT bin `bin`.
    [[nodiscard]] double binFrequencyHz(std::size_t bin) const noexcept;

    [[nodiscard]] std::size_t binCount() const noexcept { return m_fftSize / 2 + 1; }
    [[nodiscard]] std::size_t requiredLookbackFrames() const noexcept { return m_windowSize - m_hopSize; }

    // Streaming: feed samples in order; `callback` fires once per frame completed by this call.
    Result<void> process(std::span<const Sample> samples, const FrameCallback& callback);

    // Signals end of stream: with `centered`, flushes the trailing zero-padded frames a centred
    // analysis implies. Safe to call once; subsequent calls are a no-op.
    Result<void> finish(const FrameCallback& callback);

private:
    StftProcessor() = default;

    void emitReadyFrames(const FrameCallback& callback);
    void emitOneFrame(const FrameCallback& callback);

    std::unique_ptr<RealFft>         m_fft;
    std::vector<float>               m_window;             // size m_windowSize
    std::vector<float>               m_frameScratch;       // size m_fftSize; tail beyond windowSize is always 0
    std::vector<std::complex<float>> m_spectrumScratch;    // size m_fftSize/2+1
    std::vector<float>               m_magnitudeScratch;    // size m_fftSize/2+1
    std::vector<float>               m_pending;            // buffered raw samples awaiting frame formation

    std::size_t     m_fftSize    = 0;
    std::size_t     m_hopSize    = 0;
    std::size_t     m_windowSize = 0;
    SampleRate      m_sampleRate = 0;
    SpectrumScaling m_scaling    = SpectrumScaling::Amplitude;
    bool            m_centered   = true;

    double m_coherentGain        = 1.0;
    double m_noisePowerBandwidth = 1.0;

    std::size_t m_nextFrameIndex = 0;
    bool        m_finished       = false;
};

}  // namespace aud::fft

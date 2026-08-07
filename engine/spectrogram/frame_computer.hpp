#pragma once

// Computes one centred STFT analysis frame at an arbitrary sample position (M07's tile generator
// and point-query both need this: tile columns are accessed at level-dependent, non-sequential hop
// positions, and a point-query is a single one-off frame at whatever time the cursor is over —
// neither matches aud::fft::StftProcessor's sequential-streaming design, which assumes the caller
// walks forward from position 0. Re-deriving StftProcessor's whole pending-buffer state from track
// start for every random access would be O(track length) per access; this instead extracts exactly
// the windowSize samples a centred frame needs (zero-padding past the buffer's edges, same
// convention as StftProcessor's `centered=true`) and runs window+FFT+scaling directly.

#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "../fft/real_fft.hpp"
#include "../fft/scaling.hpp"
#include "../fft/simd_kernels.hpp"
#include "../fft/windows.hpp"
#include "../util/audio_buffer.hpp"
#include "../util/audio_types.hpp"
#include "../util/result.hpp"

namespace aud::spectrogram {

class FrameComputer {
public:
    static Result<FrameComputer> create(std::size_t fftSize, aud::fft::WindowType window,
                                         aud::fft::SpectrumScaling scaling, SampleRate sampleRate);

    FrameComputer(FrameComputer&&) noexcept            = default;
    FrameComputer& operator=(FrameComputer&&) noexcept = default;
    FrameComputer(const FrameComputer&)                = delete;
    FrameComputer& operator=(const FrameComputer&)     = delete;

    // Extracts fftSize samples of `buffer`'s channel `ch` centred on `centerFrame` (covering
    // [centerFrame - fftSize/2, centerFrame + fftSize/2)), zero-padding any part outside
    // [0, buffer.frameCount()), then window+FFT+scaling. Returned span (fftSize/2+1 magnitudes) is
    // owned scratch, valid until the next call.
    [[nodiscard]] std::span<const float> computeFrame(const AudioBuffer& buffer, ChannelIndex ch,
                                                       FrameIndex centerFrame);

    [[nodiscard]] std::size_t fftSize() const noexcept { return m_fftSize; }
    [[nodiscard]] std::size_t binCount() const noexcept { return m_fftSize / 2 + 1; }
    [[nodiscard]] double      binFrequencyHz(std::size_t bin) const noexcept {
        return static_cast<double>(bin) * static_cast<double>(m_sampleRate) / static_cast<double>(m_fftSize);
    }

private:
    FrameComputer() = default;

    std::unique_ptr<aud::fft::RealFft> m_fft;
    std::vector<float>                 m_window;
    std::vector<float>                 m_rawScratch;       // fftSize samples read from the buffer
    std::vector<float>                 m_windowedScratch;  // fftSize, windowed
    std::vector<std::complex<float>>   m_spectrumScratch;  // fftSize/2+1
    std::vector<float>                 m_magnitudeScratch; // fftSize/2+1

    std::size_t               m_fftSize    = 0;
    SampleRate                m_sampleRate = 0;
    aud::fft::SpectrumScaling m_scaling    = aud::fft::SpectrumScaling::Amplitude;
    double                    m_coherentGain        = 1.0;
    double                    m_noisePowerBandwidth = 1.0;
};

}  // namespace aud::spectrogram

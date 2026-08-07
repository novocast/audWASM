#include "frame_computer.hpp"

#include <algorithm>
#include <cstring>

namespace aud::spectrogram {

Result<FrameComputer> FrameComputer::create(std::size_t fftSize, aud::fft::WindowType window,
                                             aud::fft::SpectrumScaling scaling, SampleRate sampleRate) {
    if (!aud::fft::isSupportedFftSize(fftSize)) {
        return Error{ErrorCode::InvalidArgument, "spectrogram.frame_computer", "unsupported fftSize"};
    }
    if (sampleRate == 0) {
        return Error{ErrorCode::InvalidArgument, "spectrogram.frame_computer", "sampleRate must be > 0"};
    }

    AUD_TRY_ASSIGN(fftHandle, aud::fft::RealFft::create(fftSize));

    FrameComputer computer;
    computer.m_fft         = std::move(fftHandle);
    computer.m_fftSize     = fftSize;
    computer.m_sampleRate  = sampleRate;
    computer.m_scaling     = scaling;

    computer.m_window.resize(fftSize);
    aud::fft::generateWindow(window, /*periodic=*/true, /*kaiserBeta=*/8.6, computer.m_window);
    computer.m_coherentGain        = aud::fft::coherentGain(computer.m_window);
    computer.m_noisePowerBandwidth = aud::fft::noisePowerBandwidth(computer.m_window);

    computer.m_rawScratch.assign(fftSize, 0.0f);
    computer.m_windowedScratch.assign(fftSize, 0.0f);
    computer.m_spectrumScratch.resize(fftSize / 2 + 1);
    computer.m_magnitudeScratch.resize(fftSize / 2 + 1);

    return computer;
}

std::span<const float> FrameComputer::computeFrame(const AudioBuffer& buffer, ChannelIndex ch,
                                                    FrameIndex centerFrame) {
    const auto halfWindow = static_cast<FrameIndex>(m_fftSize / 2);
    const FrameIndex wantBegin = centerFrame - halfWindow;
    const FrameIndex wantEnd   = wantBegin + static_cast<FrameIndex>(m_fftSize);

    std::fill(m_rawScratch.begin(), m_rawScratch.end(), 0.0f);

    const FrameIndex bufferEnd  = buffer.frameCount();
    const FrameIndex readBegin  = std::max<FrameIndex>(wantBegin, 0);
    const FrameIndex readEnd    = std::min<FrameIndex>(wantEnd, bufferEnd);
    if (readEnd > readBegin) {
        const std::size_t destOffset = static_cast<std::size_t>(readBegin - wantBegin);
        const std::size_t count      = static_cast<std::size_t>(readEnd - readBegin);
        std::span<float>  dest(m_rawScratch.data() + destOffset, count);
        // Reads never fail for a valid channel/in-bounds range constructed above; if it somehow
        // does (corrupt buffer state), leave the zero-fill in place rather than propagating — a
        // silent-zero frame is a safe degraded result for a display feature.
        (void)buffer.read(ch, FrameRange{readBegin, readEnd}, dest);
    }

    aud::fft::windowMultiply(m_rawScratch, m_window, m_windowedScratch);
    m_fft->forward(m_windowedScratch, m_spectrumScratch);
    aud::fft::magnitude(m_spectrumScratch, m_magnitudeScratch);
    aud::fft::applyScaling(m_magnitudeScratch, m_scaling, m_fftSize, m_coherentGain, m_noisePowerBandwidth,
                            static_cast<double>(m_sampleRate));

    return m_magnitudeScratch;
}

}  // namespace aud::spectrogram

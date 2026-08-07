#include "stft.hpp"

namespace aud::fft {

Result<StftProcessor> StftProcessor::create(const StftConfig& config, SampleRate sampleRate) {
    if (!isSupportedFftSize(config.fftSize)) {
        return Error{ErrorCode::InvalidArgument, "fft.stft", "unsupported fftSize"};
    }
    const std::size_t windowSize = config.windowSize == 0 ? config.fftSize : config.windowSize;
    if (windowSize == 0 || windowSize > config.fftSize) {
        return Error{ErrorCode::InvalidArgument, "fft.stft", "windowSize must be in (0, fftSize]"};
    }
    if (config.zeroPadded && windowSize == config.fftSize) {
        return Error{ErrorCode::InvalidArgument, "fft.stft", "zeroPadded requires windowSize < fftSize"};
    }
    if (!config.zeroPadded && windowSize != config.fftSize) {
        return Error{ErrorCode::InvalidArgument, "fft.stft", "windowSize must equal fftSize unless zeroPadded"};
    }
    if (config.hopSize == 0 || config.hopSize > windowSize) {
        return Error{ErrorCode::InvalidArgument, "fft.stft", "hopSize must be in (0, windowSize]"};
    }
    if (sampleRate == 0) {
        return Error{ErrorCode::InvalidArgument, "fft.stft", "sampleRate must be > 0"};
    }

    AUD_TRY_ASSIGN(fftHandle, RealFft::create(config.fftSize));

    StftProcessor processor;
    processor.m_fft         = std::move(fftHandle);
    processor.m_fftSize     = config.fftSize;
    processor.m_hopSize     = config.hopSize;
    processor.m_windowSize  = windowSize;
    processor.m_sampleRate  = sampleRate;
    processor.m_scaling     = config.scaling;
    processor.m_centered    = config.centered;

    processor.m_window.resize(windowSize);
    generateWindow(config.window, /*periodic=*/true, /*kaiserBeta=*/8.6, processor.m_window);
    processor.m_coherentGain        = coherentGain(processor.m_window);
    processor.m_noisePowerBandwidth = noisePowerBandwidth(processor.m_window);

    // Tail beyond windowSize (present when zeroPadded) stays zero for the processor's entire
    // lifetime — emitOneFrame() only ever writes [0, windowSize).
    processor.m_frameScratch.assign(config.fftSize, 0.0f);
    processor.m_spectrumScratch.resize(config.fftSize / 2 + 1);
    processor.m_magnitudeScratch.resize(config.fftSize / 2 + 1);

    if (config.centered) {
        processor.m_pending.assign(config.fftSize / 2, 0.0f);
    }

    return processor;
}

std::size_t StftProcessor::frameCount(FrameIndex totalFrames) const noexcept {
    if (totalFrames <= 0) {
        return 0;
    }
    std::size_t length = static_cast<std::size_t>(totalFrames);
    if (m_centered) {
        length += 2 * (m_fftSize / 2);
    }
    if (length < m_windowSize) {
        return 0;
    }
    return (length - m_windowSize) / m_hopSize + 1;
}

double StftProcessor::frameTimeSeconds(std::size_t frameIndex) const noexcept {
    const double centerOffset = m_centered ? 0.0 : static_cast<double>(m_windowSize) / 2.0;
    return (static_cast<double>(frameIndex) * static_cast<double>(m_hopSize) + centerOffset) /
           static_cast<double>(m_sampleRate);
}

double StftProcessor::binFrequencyHz(std::size_t bin) const noexcept {
    return static_cast<double>(bin) * static_cast<double>(m_sampleRate) / static_cast<double>(m_fftSize);
}

Result<void> StftProcessor::process(std::span<const Sample> samples, const FrameCallback& callback) {
    if (m_finished) {
        return Error{ErrorCode::InvalidArgument, "fft.stft", "process() called after finish()"};
    }
    m_pending.insert(m_pending.end(), samples.begin(), samples.end());
    emitReadyFrames(callback);
    return {};
}

Result<void> StftProcessor::finish(const FrameCallback& callback) {
    if (m_finished) {
        return {};
    }
    m_finished = true;
    if (m_centered) {
        m_pending.insert(m_pending.end(), m_fftSize / 2, 0.0f);
    }
    emitReadyFrames(callback);
    // A final partial frame (fewer than windowSize samples buffered) can't be formed and is
    // dropped — matches librosa/scipy behaviour for a stream whose length isn't an exact multiple
    // of the hop.
    m_pending.clear();
    return {};
}

void StftProcessor::emitReadyFrames(const FrameCallback& callback) {
    while (m_pending.size() >= m_windowSize) {
        emitOneFrame(callback);
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(m_hopSize));
    }
}

void StftProcessor::emitOneFrame(const FrameCallback& callback) {
    windowMultiply(std::span<const float>(m_pending.data(), m_windowSize), m_window,
                   std::span<float>(m_frameScratch.data(), m_windowSize));

    m_fft->forward(m_frameScratch, m_spectrumScratch);
    magnitude(m_spectrumScratch, m_magnitudeScratch);
    applyScaling(m_magnitudeScratch, m_scaling, m_fftSize, m_coherentGain, m_noisePowerBandwidth,
                 static_cast<double>(m_sampleRate));

    StftFrame frame;
    frame.frameIndex   = m_nextFrameIndex++;
    frame.bins         = m_magnitudeScratch;
    frame.complexBins  = m_spectrumScratch;
    callback(frame);
}

}  // namespace aud::fft

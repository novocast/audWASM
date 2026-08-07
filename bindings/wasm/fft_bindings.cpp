// Embind surface for M06's FFT/STFT engine. Mirrors waveform_bindings.cpp's shape: a thin
// JS-facing handle wrapping the engine type, bulk {ptr,length}/{ptr,binCount} handoffs rather than
// per-bin calls (M01 binding convention), construction via a static factory since StftProcessor's
// own factory can fail and Embind constructors can't report that without exceptions.
//
// `process()`/`finish()` accumulate every frame completed by that call into one contiguous buffer
// this handle owns (frame 0's bins, then frame 1's, ...) and hand back a heap view over it — a
// bounded batch, not the "give me the whole STFT" API the M06 design doc rules out. Callers drive
// this incrementally (feed a chunk, read its frames, repeat) exactly like WaveformHandle.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../../engine/fft/stft.hpp"

using emscripten::val;

namespace bindings {

namespace {

val errorToVal(const aud::Error& error) {
    val out = val::object();
    out.set("ok", false);
    out.set("code", std::string(aud::toString(error.code)));
    out.set("detail", error.detail);
    return out;
}

}  // namespace

class StftHandle {
public:
    // windowType: 0=Rectangular,1=Hann,2=Hamming,3=Blackman,4=BlackmanHarris,5=Kaiser
    // (aud::fft::WindowType). scaling: 0=Raw,1=Amplitude,2=Power,3=PowerDensity
    // (aud::fft::SpectrumScaling).
    static std::unique_ptr<StftHandle> create(std::uint32_t fftSize, std::uint32_t hopSize,
                                               std::uint32_t windowType, std::uint32_t scaling,
                                               bool centered, std::uint32_t sampleRate) {
        aud::fft::StftConfig config;
        config.fftSize  = fftSize;
        config.hopSize  = hopSize;
        config.window   = static_cast<aud::fft::WindowType>(windowType);
        config.scaling  = static_cast<aud::fft::SpectrumScaling>(scaling);
        config.centered = centered;

        auto result = aud::fft::StftProcessor::create(config, sampleRate);
        if (!result.has_value()) {
            return nullptr;
        }
        return std::unique_ptr<StftHandle>(new StftHandle(std::move(result).value()));
    }

    std::uint32_t binCount() const { return static_cast<std::uint32_t>(m_processor.binCount()); }
    double        frameTimeSeconds(std::uint32_t frameIndex) const { return m_processor.frameTimeSeconds(frameIndex); }
    double        binFrequencyHz(std::uint32_t bin) const { return m_processor.binFrequencyHz(bin); }
    double        frameCountFor(double totalFrames) const {
        return static_cast<double>(m_processor.frameCount(static_cast<aud::FrameIndex>(totalFrames)));
    }

    // `ptr` points at `length` planar float samples already copied into the WASM heap.
    val process(std::uintptr_t ptr, std::size_t length) {
        const auto* samples = reinterpret_cast<const float*>(ptr);
        auto        result  = m_processor.process(std::span<const aud::Sample>(samples, length), frameSink());
        return framesToVal(result);
    }

    val finish() {
        auto result = m_processor.finish(frameSink());
        return framesToVal(result);
    }

private:
    explicit StftHandle(aud::fft::StftProcessor processor) : m_processor(std::move(processor)) {}

    aud::fft::FrameCallback frameSink() {
        m_scratch.clear();
        return [this](const aud::fft::StftFrame& frame) {
            m_scratch.insert(m_scratch.end(), frame.bins.begin(), frame.bins.end());
        };
    }

    val framesToVal(const aud::Result<void>& result) {
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        const std::size_t bins = m_processor.binCount();
        val               out  = val::object();
        out.set("ok", true);
        out.set("ptr", static_cast<double>(reinterpret_cast<std::uintptr_t>(m_scratch.data())));
        out.set("binCount", static_cast<std::uint32_t>(bins));
        out.set("frameCount", static_cast<std::uint32_t>(bins == 0 ? 0 : m_scratch.size() / bins));
        return out;
    }

    aud::fft::StftProcessor m_processor;
    std::vector<float>      m_scratch;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_fft) {
    emscripten::class_<bindings::StftHandle>("Stft")
        .class_function("create", &bindings::StftHandle::create)
        .function("process", &bindings::StftHandle::process)
        .function("finish", &bindings::StftHandle::finish)
        .function("binCount", &bindings::StftHandle::binCount)
        .function("frameTimeSeconds", &bindings::StftHandle::frameTimeSeconds)
        .function("binFrequencyHz", &bindings::StftHandle::binFrequencyHz)
        .function("frameCountFor", &bindings::StftHandle::frameCountFor);
}

// Embind surface for M04's waveform generator. Deliberately drives WaveformStore's own
// reset()/appendChunk()/markComplete() directly rather than going through the Analyzer/
// WaveformAnalyzer interface — two independent reasons, not just one:
//  1. The shared per-chunk analyser dispatch that would make going through Analyzer meaningful
//     (one call site feeding every registered Analyzer as chunks land) is M20's job; until then
//     this binding *is* the driver, so the indirection buys nothing.
//  2. aud_core is built PRIVATE -fno-rtti (M00 §2) but this Embind target is not — directly naming
//     and constructing the concrete WaveformAnalyzer from this (RTTI-enabled) TU would ask the
//     linker for "typeinfo for WaveformAnalyzer", which -fno-rtti never emits, and fail to link
//     (see waveform_analyzer.hpp's makeWaveformAnalyzer() comment for the full explanation; that
//     factory exists for callers, like the unit tests, that do need the Analyzer interface itself).
// processAvailableChunks() is safe to call repeatedly as decode progresses (it only reduces chunks
// appended since the last call), so a caller polling it after each feed()/finish() on the driving
// DecodeSession gets a progressively filling waveform.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "../../engine/util/audio_buffer.hpp"
#include "../../engine/waveform/waveform_bin.hpp"
#include "../../engine/waveform/waveform_store.hpp"

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

val okVal() {
    val out = val::object();
    out.set("ok", true);
    return out;
}

// { ok, ptr, binCount } — `ptr` points at a WaveformBin[binCount] (4 floats each: min, max, rms,
// absPeak), a bulk {ptr,length} handoff per M01's binding convention rather than per-bin calls.
val binsToVal(std::span<const aud::waveform::WaveformBin> bins) {
    val out = val::object();
    out.set("ok", true);
    out.set("ptr", static_cast<double>(reinterpret_cast<std::uintptr_t>(bins.data())));
    out.set("binCount", static_cast<std::uint32_t>(bins.size()));
    return out;
}

}  // namespace

// Thin JS-facing wrapper around aud::waveform::WaveformStore, driven directly against a
// non-owning aud::AudioBuffer* (the same DecodeSessionHandle::audioBufferHandle() handoff pattern
// TransportHandle uses — see playback_bindings.cpp). The DecodeSession must outlive this handle.
class WaveformHandle {
public:
    static std::unique_ptr<WaveformHandle> create(std::uintptr_t audioBufferHandle) {
        const auto* buffer = reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle);
        if (buffer == nullptr) {
            return nullptr;
        }
        return std::unique_ptr<WaveformHandle>(new WaveformHandle(buffer));
    }

    // Reduces every AudioBuffer chunk appended since the last call. Call again after feed()ing
    // more bytes into the driving DecodeSession to keep the waveform progressing alongside decode.
    val processAvailableChunks() {
        const std::size_t      chunkCount = m_buffer->chunkCount();
        const aud::ChannelIndex channels  = m_buffer->channelCount();

        while (m_nextChunk < chunkCount) {
            for (aud::ChannelIndex ch = 0; ch < channels; ++ch) {
                m_store.appendChunk(ch, m_buffer->chunk(ch, m_nextChunk));
            }
            ++m_nextChunk;
        }
        return okVal();
    }

    // Signals decode is complete; call once after the driving DecodeSession's finish().
    val finish() {
        m_store.markComplete();
        return okVal();
    }

    std::uint32_t channelCount() const { return m_store.channelCount(); }
    bool          isComplete() const { return m_store.isComplete(); }

    val levelZeroBins(std::uint32_t channel) const { return binsToVal(m_store.bins(channel)); }

    val monoSumBins() {
        auto result = m_store.monoSumBins(*m_buffer);
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        return binsToVal(result.value());
    }

    val midBins() {
        auto result = m_store.midBins(*m_buffer);
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        return binsToVal(result.value());
    }

    val sideBins() {
        auto result = m_store.sideBins(*m_buffer);
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        return binsToVal(result.value());
    }

    // channelsMode: 0 = PerChannel, 1 = MonoSum, 2 = MidSide (aud::waveform::ChannelSelector).
    val query(std::uint32_t channelsMode, double rangeBegin, double rangeEnd, std::uint32_t binCount) {
        aud::waveform::WaveformRequest request;
        request.channels = static_cast<aud::waveform::ChannelSelector>(channelsMode);
        request.range    = aud::FrameRange{static_cast<aud::FrameIndex>(rangeBegin), static_cast<aud::FrameIndex>(rangeEnd)};
        request.binCount = binCount;

        auto result = m_store.query(request, m_buffer);
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        const auto& view = result.value();
        val         out  = val::object();
        out.set("ok", true);
        out.set("ptr", static_cast<double>(reinterpret_cast<std::uintptr_t>(view.data)));
        out.set("channels", view.channels);
        out.set("binCount", view.binCount);
        out.set("framesPerBin", view.framesPerBin);
        out.set("isComplete", view.isComplete);
        return out;
    }

private:
    explicit WaveformHandle(const aud::AudioBuffer* buffer) : m_buffer(buffer) {
        m_store.reset(buffer->channelCount());
    }

    const aud::AudioBuffer*      m_buffer;
    aud::waveform::WaveformStore m_store;
    std::size_t                  m_nextChunk = 0;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_waveform) {
    emscripten::class_<bindings::WaveformHandle>("Waveform")
        .class_function("create", &bindings::WaveformHandle::create)
        .function("processAvailableChunks", &bindings::WaveformHandle::processAvailableChunks)
        .function("finish", &bindings::WaveformHandle::finish)
        .function("channelCount", &bindings::WaveformHandle::channelCount)
        .function("isComplete", &bindings::WaveformHandle::isComplete)
        .function("levelZeroBins", &bindings::WaveformHandle::levelZeroBins)
        .function("monoSumBins", &bindings::WaveformHandle::monoSumBins)
        .function("midBins", &bindings::WaveformHandle::midBins)
        .function("sideBins", &bindings::WaveformHandle::sideBins)
        .function("query", &bindings::WaveformHandle::query);
}

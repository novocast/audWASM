// Embind surface for M07's tile-based spectrogram. Mirrors waveform_bindings.cpp's shape: a thin
// JS-facing handle driven directly against a non-owning aud::AudioBuffer* (the same
// DecodeSessionHandle::audioBufferHandle() handoff pattern WaveformHandle/TransportHandle use),
// bulk {ptr,length}-style handoffs (M01 binding convention) rather than per-pixel calls.
//
// Unlike WaveformHandle (driven directly from the main-thread decode loop), this handle is meant to
// be constructed inside the dedicated spectrogram worker (M07 "worker/PCM ownership" decision) —
// the worker builds its own AudioBuffer from a copy of the decoded PCM and this handle wraps that
// copy, not the main thread's. Nothing about the binding itself assumes which thread it runs on.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../engine/spectrogram/overview.hpp"
#include "../../engine/spectrogram/point_query.hpp"
#include "../../engine/spectrogram/tile.hpp"
#include "../../engine/spectrogram/tile_cache.hpp"
#include "../../engine/util/audio_buffer.hpp"

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

}  // namespace

// Builds a standalone aud::AudioBuffer from planar PCM copied into the heap, entirely independent
// of DecodeSession. Exists for exactly one reason: the spectrogram worker (M07 "worker/PCM
// ownership" decision) gets its own copy of the decoded PCM and its own WASM module instance rather
// than sharing the main thread's AudioBuffer (no SharedArrayBuffer is available — no COOP/COEP —
// and cross-thread engine ownership is M20's problem, not M07's). `SpectrogramHandle::create()`
// takes this handle's `audioBufferHandle()` exactly the way it takes a DecodeSession's.
//
// `appendPlanar`'s `channelPtrsPtr` points at `channelCount` heap pointers (each itself pointing at
// `frames` float32 samples already copied into the heap) — a pointer-to-pointers, the natural
// extension of this codebase's bulk {ptr,length} convention to "one bulk transfer per channel,
// handed over in one call" rather than one call per channel (which would require the caller to
// pre-know channel ordering matters or split appends across calls, needlessly).
class PcmBufferHandle {
public:
    static std::unique_ptr<PcmBufferHandle> create(std::uint32_t sampleRate, std::uint32_t channels) {
        auto result = aud::AudioBuffer::create(sampleRate, channels);
        if (!result.has_value()) {
            return nullptr;
        }
        return std::unique_ptr<PcmBufferHandle>(new PcmBufferHandle(std::move(result).value()));
    }

    val appendPlanar(std::uintptr_t channelPtrsPtr, std::uint32_t channelCount, std::size_t frames) {
        if (channelCount != m_buffer.channelCount()) {
            return errorToVal(aud::Error{aud::ErrorCode::InvalidArgument, "bindings.spectrogram",
                                          "channelCount does not match the buffer's channel count"});
        }
        const auto* ptrs = reinterpret_cast<const std::uintptr_t*>(channelPtrsPtr);
        std::vector<std::span<const aud::Sample>> planar(channelCount);
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            planar[ch] = std::span<const aud::Sample>(reinterpret_cast<const float*>(ptrs[ch]), frames);
        }
        auto result = m_buffer.append(planar, frames);
        return errorToVal(result);
    }

    // Opaque handoff, same convention as DecodeSessionHandle::audioBufferHandle(). The
    // SpectrogramHandle built from this must not outlive `this`.
    std::uintptr_t audioBufferHandle() const { return reinterpret_cast<std::uintptr_t>(&m_buffer); }

private:
    explicit PcmBufferHandle(aud::AudioBuffer buffer) : m_buffer(std::move(buffer)) {}

    static val errorToVal(const aud::Result<void>& result) {
        val out = val::object();
        out.set("ok", result.has_value());
        if (!result.has_value()) {
            out.set("code", std::string(aud::toString(result.error().code)));
            out.set("detail", result.error().detail);
        }
        return out;
    }

    aud::AudioBuffer m_buffer;
};

class SpectrogramHandle {
public:
    static std::unique_ptr<SpectrogramHandle> create(std::uintptr_t audioBufferHandle, double byteBudget) {
        const auto* buffer = reinterpret_cast<const aud::AudioBuffer*>(audioBufferHandle);
        if (buffer == nullptr) {
            return nullptr;
        }
        const std::size_t budget = byteBudget > 0.0 ? static_cast<std::size_t>(byteBudget)
                                                      : aud::spectrogram::kDefaultTileCacheByteBudget;
        return std::unique_ptr<SpectrogramHandle>(new SpectrogramHandle(buffer, budget));
    }

    // window: aud::fft::WindowType, scaling: aud::fft::SpectrumScaling, freqAxis:
    // aud::spectrogram::FreqAxis, decimation: aud::spectrogram::Decimation (all raw integer values
    // of those enums, same convention as fft_bindings.cpp's StftHandle::create()).
    val setConfig(std::uint32_t fftSize, std::uint32_t window, std::uint32_t scaling, std::uint32_t freqAxis,
                  std::uint32_t decimation, float minHz, float floorDb, float ceilDb) {
        aud::spectrogram::TileConfig config;
        config.fftSize    = fftSize;
        config.window     = window;
        config.scaling    = scaling;
        config.freqAxis   = freqAxis;
        config.decimation = decimation;
        config.minHz      = minHz;
        config.floorDb    = floorDb;
        config.ceilDb     = ceilDb;

        auto result = m_cache.setConfig(config, m_buffer->sampleRate());
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        m_config = config;
        m_overviews.assign(m_buffer->channelCount(), std::nullopt);

        val out = okVal();
        out.set("configHash", m_cache.currentConfigHash());
        return out;
    }

    std::uint32_t currentConfigHash() const { return m_cache.currentConfigHash(); }

    // { ok, ptr, byteLength, floorDb, ceilDb } — ptr points at kTileWidth*kTileHeight quantised
    // bytes (row 0 = lowest frequency; see tile.hpp).
    val requestTile(std::uint32_t level, std::uint32_t tileX, std::uint32_t channel) {
        aud::spectrogram::TileKey key;
        key.level      = level;
        key.tileX      = tileX;
        key.channel     = channel;
        key.configHash  = m_cache.currentConfigHash();

        auto result = m_cache.request(*m_buffer, key);
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        const aud::spectrogram::TileData* tile = result.value();

        val out = okVal();
        out.set("ptr", static_cast<double>(reinterpret_cast<std::uintptr_t>(tile->pixels.data())));
        out.set("byteLength", static_cast<std::uint32_t>(tile->pixels.size()));
        out.set("floorDb", tile->floorDb);
        out.set("ceilDb", tile->ceilDb);
        return out;
    }

    void invalidateConfig(std::uint32_t staleConfigHash) { m_cache.invalidateConfig(staleConfigHash); }

    std::uint32_t cacheTileCount() const { return static_cast<std::uint32_t>(m_cache.tileCount()); }
    double        cacheCurrentBytes() const { return static_cast<double>(m_cache.currentBytes()); }
    double        cacheByteBudget() const { return static_cast<double>(m_cache.byteBudget()); }

    // { ok, ptr, width, height, floorDb, ceilDb } — computed once per channel per config, cached
    // (M07 "The overview level": eager, resident, computed once).
    val overview(std::uint32_t channel) {
        if (channel >= m_overviews.size()) {
            return errorToVal(aud::Error{aud::ErrorCode::InvalidArgument, "bindings.spectrogram", "channel out of range"});
        }
        if (!m_overviews[channel].has_value()) {
            auto result = aud::spectrogram::computeOverviewStrip(*m_buffer, channel, m_config);
            if (!result.has_value()) {
                return errorToVal(result.error());
            }
            m_overviews[channel] = std::move(result).value();
        }
        const auto& strip = *m_overviews[channel];

        val out = okVal();
        out.set("ptr", static_cast<double>(reinterpret_cast<std::uintptr_t>(strip.pixels.data())));
        out.set("width", strip.width);
        out.set("height", strip.height);
        out.set("floorDb", strip.floorDb);
        out.set("ceilDb", strip.ceilDb);
        return out;
    }

    // { ok, frequencyHz, magnitudeDb } — bypasses the tile cache entirely (M07 "Cursor readout").
    val queryPoint(std::uint32_t channel, double timeSeconds, double targetHz) {
        auto result = aud::spectrogram::queryPoint(*m_buffer, channel, timeSeconds, targetHz, m_config);
        if (!result.has_value()) {
            return errorToVal(result.error());
        }
        val out = okVal();
        out.set("frequencyHz", result.value().frequencyHz);
        out.set("magnitudeDb", result.value().magnitudeDb);
        return out;
    }

private:
    SpectrogramHandle(const aud::AudioBuffer* buffer, std::size_t byteBudget)
        : m_buffer(buffer), m_cache(byteBudget), m_overviews(buffer->channelCount()) {}

    const aud::AudioBuffer*        m_buffer;
    aud::spectrogram::TileConfig   m_config;
    aud::spectrogram::TileCache    m_cache;
    std::vector<std::optional<aud::spectrogram::OverviewStrip>> m_overviews;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_spectrogram) {
    emscripten::class_<bindings::PcmBufferHandle>("PcmBuffer")
        .class_function("create", &bindings::PcmBufferHandle::create)
        .function("appendPlanar", &bindings::PcmBufferHandle::appendPlanar)
        .function("audioBufferHandle", &bindings::PcmBufferHandle::audioBufferHandle);

    emscripten::class_<bindings::SpectrogramHandle>("Spectrogram")
        .class_function("create", &bindings::SpectrogramHandle::create)
        .function("setConfig", &bindings::SpectrogramHandle::setConfig)
        .function("currentConfigHash", &bindings::SpectrogramHandle::currentConfigHash)
        .function("requestTile", &bindings::SpectrogramHandle::requestTile)
        .function("invalidateConfig", &bindings::SpectrogramHandle::invalidateConfig)
        .function("cacheTileCount", &bindings::SpectrogramHandle::cacheTileCount)
        .function("cacheCurrentBytes", &bindings::SpectrogramHandle::cacheCurrentBytes)
        .function("cacheByteBudget", &bindings::SpectrogramHandle::cacheByteBudget)
        .function("overview", &bindings::SpectrogramHandle::overview)
        .function("queryPoint", &bindings::SpectrogramHandle::queryPoint);
}

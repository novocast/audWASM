// Embind surface. Deliberately small (M01 decision): objects/enums/optional fields where Embind's
// structure earns its ~40-60KB size cost, raw {ptr,length} views for anything bulk. Nothing here
// touches <emscripten.h> beyond emscripten/bind.h and emscripten/val.h, which are bindings-layer
// concerns, not engine ones (engine/ itself must never include Emscripten headers — M00 §4).

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <string>

#include "../../engine/decoder/decode_session.hpp"
#include "../../engine/util/diagnostics.hpp"

using emscripten::val;

namespace bindings {

std::string engineVersion() {
    const aud::BuildInfo info = aud::buildInfo();
    return info.version;
}

val engineBuildInfo() {
    const aud::BuildInfo info = aud::buildInfo();
    val                  out  = val::object();
    out.set("version", info.version);
    out.set("simd", info.simd);
    out.set("threads", info.threads);
    out.set("optimisation", info.optimisation);
    return out;
}

// Returns true/pass or a { code, detail } object on failure — kept as a val rather than throwing
// (aud::core is built -fno-exceptions; this boundary layer doesn't propagate C++ exceptions from
// engine failure paths, it translates aud::Error into plain JS values).
val selfTest() {
    auto result = aud::selfTest();
    val   out   = val::object();
    if (result.has_value()) {
        out.set("pass", true);
        return out;
    }
    out.set("pass", false);
    out.set("code", std::string(aud::toString(result.error().code)));
    out.set("detail", result.error().detail);
    return out;
}

// Thin JS-facing wrapper around aud::decoder::DecodeSession. DecodeSession itself is move-only and
// its factory can fail (Result<DecodeSession>), which doesn't map onto an Embind constructor
// directly (constructors can't report failure without exceptions) — so construction happens via a
// static factory that returns nullptr on failure, and the handle is bound through
// std::unique_ptr so ownership on the JS side is explicit (see M01's TS wrapper rules).
class DecodeSessionHandle {
public:
    static std::unique_ptr<DecodeSessionHandle> create(std::uintptr_t probePtr, std::size_t probeLength) {
        auto* bytes = reinterpret_cast<const std::byte*>(probePtr);
        auto  result =
            aud::decoder::DecodeSession::create(std::span<const std::byte>(bytes, probeLength));
        if (!result.has_value()) {
            return nullptr;
        }
        return std::unique_ptr<DecodeSessionHandle>(new DecodeSessionHandle(std::move(result).value()));
    }

    // `ptr` must point into the WASM heap (JS copies its bytes there first via HEAPU8.set()) —
    // see M01 rule: bulk transfers are one call, never getSample(i)-style per-element calls.
    val feedBytes(std::uintptr_t ptr, std::size_t length) {
        auto* bytes  = reinterpret_cast<const std::byte*>(ptr);
        auto  result = m_session.feed(std::span<const std::byte>(bytes, length));
        return errorToVal(result);
    }

    val finish() {
        auto result = m_session.finish();
        return errorToVal(result);
    }

    val getStreamInfo() {
        auto result = m_session.streamInfo();
        val  out    = val::object();
        if (!result.has_value()) {
            out.set("ok", false);
            return out;
        }
        const auto& info = result.value();
        out.set("ok", true);
        out.set("sampleRate", info.sampleRate);
        out.set("channels", info.channels);
        out.set("frameCount", static_cast<double>(info.frameCount));  // -1 sentinel if unknown
        out.set("codecName", info.codecName);
        out.set("bitDepth", info.bitDepth);
        out.set("nominalBitrate", info.nominalBitrate);
        out.set("isLossy", info.isLossy);
        out.set("isEstimate", info.isEstimate);
        out.set("encoderDelayFrames", info.encoderDelayFrames);
        out.set("encoderPaddingFrames", info.encoderPaddingFrames);
        return out;
    }

    double getDecodedFrameCount() const {
        const auto* buffer = m_session.buffer();
        return buffer == nullptr ? 0.0 : static_cast<double>(buffer->frameCount());
    }

    // Opaque handle to the underlying aud::AudioBuffer, for TransportHandle::attachSource() (see
    // playback_bindings.cpp). Deliberately a raw pointer-as-integer rather than a second Embind
    // wrapper type: the AudioBuffer is non-owning here (DecodeSession keeps it alive) and this is
    // purely a cross-binding-file handoff, the same "raw {ptr,length}/pointer for bulk data" pattern
    // used everywhere else at this boundary (M01).
    std::uintptr_t audioBufferHandle() const {
        return reinterpret_cast<std::uintptr_t>(m_session.buffer());
    }

    val getDiagnostics() {
        val    array = val::array();
        auto   diags = m_session.takeDiagnostics();
        for (std::size_t i = 0; i < diags.size(); ++i) {
            const auto& d   = diags[i];
            val         obj = val::object();
            obj.set("severity", static_cast<int>(d.severity));
            obj.set("code", std::string(aud::toString(d.code)));
            obj.set("byteOffset", static_cast<double>(d.byteOffset));
            obj.set("frameIndex", static_cast<double>(d.frameIndex));
            obj.set("message", d.message);
            array.set(i, obj);
        }
        return array;
    }

private:
    explicit DecodeSessionHandle(aud::decoder::DecodeSession session) : m_session(std::move(session)) {}

    static val errorToVal(const aud::Result<void>& result) {
        val out = val::object();
        out.set("ok", result.has_value());
        if (!result.has_value()) {
            out.set("code", std::string(aud::toString(result.error().code)));
            out.set("detail", result.error().detail);
        }
        return out;
    }

    aud::decoder::DecodeSession m_session;
};

}  // namespace bindings

EMSCRIPTEN_BINDINGS(aud_core) {
    emscripten::function("engineVersion", &bindings::engineVersion);
    emscripten::function("engineBuildInfo", &bindings::engineBuildInfo);
    emscripten::function("selfTest", &bindings::selfTest);

    emscripten::class_<bindings::DecodeSessionHandle>("DecodeSession")
        .class_function("create", &bindings::DecodeSessionHandle::create)
        .function("feedBytes", &bindings::DecodeSessionHandle::feedBytes)
        .function("finish", &bindings::DecodeSessionHandle::finish)
        .function("getStreamInfo", &bindings::DecodeSessionHandle::getStreamInfo)
        .function("getDecodedFrameCount", &bindings::DecodeSessionHandle::getDecodedFrameCount)
        .function("getDiagnostics", &bindings::DecodeSessionHandle::getDiagnostics)
        .function("audioBufferHandle", &bindings::DecodeSessionHandle::audioBufferHandle);
}

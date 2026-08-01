#include "diagnostics.hpp"

#include <array>
#include <cstdio>

#include "audio_buffer.hpp"
#include "version.hpp"

namespace aud {

BuildInfo buildInfo() noexcept {
    BuildInfo info;

    std::array<char, 32> versionBuf{};
    std::snprintf(versionBuf.data(), versionBuf.size(), "%d.%d.%d", kEngineVersion.major, kEngineVersion.minor,
                  kEngineVersion.patch);
    info.version = versionBuf.data();

#if defined(__wasm_simd128__) || defined(__AVX2__) || defined(__ARM_NEON)
    info.simd = true;
#else
    info.simd = false;
#endif

#if defined(__EMSCRIPTEN_PTHREADS__)
    info.threads = true;
#else
    info.threads = false;  // v1 ships single-threaded WASM; see M00 §5
#endif

#if defined(NDEBUG)
    info.optimisation = "Release";
#else
    info.optimisation = "Debug";
#endif

    return info;
}

Result<void> selfTest() {
    // Result<T> move semantics round-trip.
    Result<int> movedInt(42);
    Result<int> destination(std::move(movedInt));
    if (!destination.has_value() || destination.value() != 42) {
        return Error{ErrorCode::Unknown, "selftest", "Result<T> move round-trip failed"};
    }

    // AudioBuffer create/append/read round-trip.
    AUD_TRY_ASSIGN(buffer, AudioBuffer::create(44100, 2));

    constexpr std::size_t     kFrames = 128;
    std::vector<Sample>       left(kFrames), right(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        left[i]  = static_cast<float>(i) / static_cast<float>(kFrames);
        right[i] = -left[i];
    }
    std::vector<std::span<const Sample>> planar{left, right};
    AUD_TRY(buffer.append(planar, kFrames));

    if (buffer.frameCount() != static_cast<FrameIndex>(kFrames)) {
        return Error{ErrorCode::Unknown, "selftest", "AudioBuffer frame count mismatch after append"};
    }

    std::vector<Sample> readBack(kFrames);
    AUD_TRY(buffer.read(0, FrameRange{0, static_cast<FrameIndex>(kFrames)}, readBack));
    if (readBack[1] != left[1]) {
        return Error{ErrorCode::Unknown, "selftest", "AudioBuffer read-back mismatch"};
    }

    return Result<void>{};
}

}  // namespace aud

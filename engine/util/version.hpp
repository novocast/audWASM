#pragma once

// Engine ABI version (M00 §8). The frontend refuses to run against a mismatched major version.
// Bump manually; there is deliberately no auto-derivation from git describe so the version is
// stable and reviewable in a diff.

#define AUD_ENGINE_VERSION_MAJOR 0
#define AUD_ENGINE_VERSION_MINOR 1
#define AUD_ENGINE_VERSION_PATCH 0

namespace aud {

struct EngineVersion {
    int major = AUD_ENGINE_VERSION_MAJOR;
    int minor = AUD_ENGINE_VERSION_MINOR;
    int patch = AUD_ENGINE_VERSION_PATCH;
};

inline constexpr EngineVersion kEngineVersion{};

}  // namespace aud

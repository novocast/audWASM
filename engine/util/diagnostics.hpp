#pragma once

// Shared by aud_cli's --self-test/--version and the Embind engineBuildInfo()/selfTest() surface
// (M01), so both entry points report identical information.

#include <string>

#include "result.hpp"

namespace aud {

struct BuildInfo {
    std::string version;      // "major.minor.patch"
    bool        simd     = false;
    bool        threads  = false;
    std::string optimisation;  // "Debug" | "Release" | "RelWithDebInfo" | "Unknown"
};

BuildInfo buildInfo() noexcept;

// Exercises a minimal cross-section of the engine (Result move semantics, AudioBuffer
// create/append/read round-trip) so a fresh build/embed can prove the pipeline works end to end
// without needing a real audio file. Returns an Error describing the first thing that failed.
Result<void> selfTest();

}  // namespace aud

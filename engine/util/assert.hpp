#pragma once

// AUD_ASSERT / aud::panic — for programmer error only. Anything a malformed input file can trigger
// must be an aud::Error, never an assertion (see M00 §2). Active in Debug and in the wasm-profile
// (RelWithDebInfo) preset; compiled out entirely in Release.

#include <string_view>

namespace aud {

// Prints a readable message (console under WASM, stderr natively) and terminates. Never returns.
[[noreturn]] void panic(std::string_view message, std::string_view file, int line) noexcept;

}  // namespace aud

#if !defined(NDEBUG) || defined(AUD_FORCE_ASSERTIONS)
#define AUD_ASSERT(cond, msg)                                                                     \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            ::aud::panic((msg), __FILE__, __LINE__);                                              \
        }                                                                                          \
    } while (0)
#else
#define AUD_ASSERT(cond, msg)                                                                     \
    do {                                                                                          \
        (void)sizeof(cond);                                                                       \
    } while (0)
#endif

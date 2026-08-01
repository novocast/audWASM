#include "platform.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace aud::platform {

void debugBreak() noexcept {
#if defined(_MSC_VER)
    __debugbreak();
#elif defined(__has_builtin)
#if __has_builtin(__builtin_debugtrap)
    __builtin_debugtrap();
#elif __has_builtin(__builtin_trap)
    __builtin_trap();
#endif
#endif
}

}  // namespace aud::platform

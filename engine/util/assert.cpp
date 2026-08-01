#include "assert.hpp"

#include <cstdio>
#include <cstdlib>

#include "platform.hpp"

namespace aud {

[[noreturn]] void panic(std::string_view message, std::string_view file, int line) noexcept {
    std::fprintf(stderr, "aud::panic at %.*s:%d: %.*s\n", static_cast<int>(file.size()), file.data(),
                 line, static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
    platform::debugBreak();
    std::abort();
}

}  // namespace aud

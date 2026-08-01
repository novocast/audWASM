#pragma once

// Level-filtered logging. Below Warn is compiled out entirely in Release builds (not just
// runtime-filtered) so log-string literals don't bloat the release WASM binary.

#include <cstdio>
#include <string_view>

namespace aud {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
};

namespace detail {

// Runtime floor; independent of the compile-time floor below. Lets a debug build quiet itself
// without recompiling.
LogLevel& logLevelFloor() noexcept;

void logImpl(LogLevel level, std::string_view domain, std::string_view message) noexcept;

}  // namespace detail

inline void setLogLevel(LogLevel level) noexcept { detail::logLevelFloor() = level; }
inline LogLevel logLevel() noexcept { return detail::logLevelFloor(); }

#if defined(NDEBUG)
inline constexpr LogLevel kCompileTimeLogFloor = LogLevel::Warn;
#else
inline constexpr LogLevel kCompileTimeLogFloor = LogLevel::Trace;
#endif

}  // namespace aud

#define AUD_LOG_AT(level, domain, message)                                                        \
    do {                                                                                          \
        if constexpr ((level) >= ::aud::kCompileTimeLogFloor) {                                   \
            if ((level) >= ::aud::logLevel()) {                                                   \
                ::aud::detail::logImpl((level), (domain), (message));                             \
            }                                                                                     \
        }                                                                                          \
    } while (0)

#define AUD_LOG_TRACE(domain, message) AUD_LOG_AT(::aud::LogLevel::Trace, domain, message)
#define AUD_LOG_DEBUG(domain, message) AUD_LOG_AT(::aud::LogLevel::Debug, domain, message)
#define AUD_LOG_INFO(domain, message)  AUD_LOG_AT(::aud::LogLevel::Info, domain, message)
#define AUD_LOG_WARN(domain, message)  AUD_LOG_AT(::aud::LogLevel::Warn, domain, message)
#define AUD_LOG_ERROR(domain, message) AUD_LOG_AT(::aud::LogLevel::Error, domain, message)

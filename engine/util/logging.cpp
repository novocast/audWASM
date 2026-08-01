#include "logging.hpp"

namespace aud::detail {

LogLevel& logLevelFloor() noexcept {
    static LogLevel floor = kCompileTimeLogFloor;
    return floor;
}

namespace {

const char* levelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

}  // namespace

void logImpl(LogLevel level, std::string_view domain, std::string_view message) noexcept {
    std::FILE* stream = (level >= LogLevel::Warn) ? stderr : stdout;
    std::fprintf(stream, "[%s] %.*s: %.*s\n", levelName(level), static_cast<int>(domain.size()),
                 domain.data(), static_cast<int>(message.size()), message.data());
}

}  // namespace aud::detail

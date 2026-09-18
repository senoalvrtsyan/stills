#pragma once
// stills/stills_Log.h — control of FFmpeg's process-global log level. The library never changes the
// level itself; host applications (and the test-suite) own that decision.

#include <cstdint>

#include "stills/detail/stills_FFmpeg.h"

namespace stills
{

enum class LogLevel : std::int8_t
{
    quiet,
    panic,
    fatal,
    error,
    warning,
    info,
    verbose,
    debug,
    trace
};

inline void setLogLevel (LogLevel level) noexcept
{
    int avLevel = AV_LOG_INFO;
    switch (level)
    {
    case LogLevel::quiet:
        avLevel = AV_LOG_QUIET;
        break;
    case LogLevel::panic:
        avLevel = AV_LOG_PANIC;
        break;
    case LogLevel::fatal:
        avLevel = AV_LOG_FATAL;
        break;
    case LogLevel::error:
        avLevel = AV_LOG_ERROR;
        break;
    case LogLevel::warning:
        avLevel = AV_LOG_WARNING;
        break;
    case LogLevel::info:
        avLevel = AV_LOG_INFO;
        break;
    case LogLevel::verbose:
        avLevel = AV_LOG_VERBOSE;
        break;
    case LogLevel::debug:
        avLevel = AV_LOG_DEBUG;
        break;
    case LogLevel::trace:
        avLevel = AV_LOG_TRACE;
        break;
    }

    av_log_set_level (avLevel);
}

[[nodiscard]] inline LogLevel getLogLevel() noexcept
{
    const int avLevel = av_log_get_level();

    if (avLevel <= AV_LOG_QUIET) return LogLevel::quiet;
    if (avLevel <= AV_LOG_PANIC) return LogLevel::panic;
    if (avLevel <= AV_LOG_FATAL) return LogLevel::fatal;
    if (avLevel <= AV_LOG_ERROR) return LogLevel::error;
    if (avLevel <= AV_LOG_WARNING) return LogLevel::warning;
    if (avLevel <= AV_LOG_INFO) return LogLevel::info;
    if (avLevel <= AV_LOG_VERBOSE) return LogLevel::verbose;
    if (avLevel <= AV_LOG_DEBUG) return LogLevel::debug;
    return LogLevel::trace;
}

} // namespace stills

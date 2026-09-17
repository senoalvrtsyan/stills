#pragma once
// stills/log.hpp — control of FFmpeg's process-global log level. The library never changes the
// level itself; host applications (and the test-suite) own that decision.

#include <cstdint>

#include "stills/detail/ffmpeg.hpp"

namespace stills {

enum class LogLevel : std::int8_t {
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

inline void set_log_level(LogLevel level) noexcept {
  int av = AV_LOG_INFO;
  switch (level) {
    case LogLevel::quiet:
      av = AV_LOG_QUIET;
      break;
    case LogLevel::panic:
      av = AV_LOG_PANIC;
      break;
    case LogLevel::fatal:
      av = AV_LOG_FATAL;
      break;
    case LogLevel::error:
      av = AV_LOG_ERROR;
      break;
    case LogLevel::warning:
      av = AV_LOG_WARNING;
      break;
    case LogLevel::info:
      av = AV_LOG_INFO;
      break;
    case LogLevel::verbose:
      av = AV_LOG_VERBOSE;
      break;
    case LogLevel::debug:
      av = AV_LOG_DEBUG;
      break;
    case LogLevel::trace:
      av = AV_LOG_TRACE;
      break;
  }
  av_log_set_level(av);
}

[[nodiscard]] inline LogLevel log_level() noexcept {
  const int av = av_log_get_level();
  if (av <= AV_LOG_QUIET) return LogLevel::quiet;
  if (av <= AV_LOG_PANIC) return LogLevel::panic;
  if (av <= AV_LOG_FATAL) return LogLevel::fatal;
  if (av <= AV_LOG_ERROR) return LogLevel::error;
  if (av <= AV_LOG_WARNING) return LogLevel::warning;
  if (av <= AV_LOG_INFO) return LogLevel::info;
  if (av <= AV_LOG_VERBOSE) return LogLevel::verbose;
  if (av <= AV_LOG_DEBUG) return LogLevel::debug;
  return LogLevel::trace;
}

}  // namespace stills

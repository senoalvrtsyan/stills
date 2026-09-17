#pragma once
// stills/error.hpp — the single error type used across the public API. No FFmpeg dependency.

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <version>

#include "stills/detail/config.hpp"

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#include <format>
#endif

namespace stills {

/// What went wrong, expressed in terms of what the caller can do about it.
enum class ErrorCode : std::uint8_t {
  invalid_argument,  ///< non-finite/negative time, bad Options, bad stream index, ...
  invalid_state,     ///< the object cannot do this: moved from, destroyed, or the wrong kind
  file_not_found,    ///< the source does not exist (AVERROR(ENOENT))
  open_failed,       ///< avformat_open_input failed for another reason (permissions, protocol, ...)
  unsupported_format,    ///< not a media container / stream info could not be read
  no_video_stream,       ///< no video stream (audio-only asset, or only cover art)
  decoder_not_found,     ///< this FFmpeg build has no decoder for the codec
  decoder_open_failed,   ///< avcodec_open2 failed
  hardware_unavailable,  ///< require_hardware was set and no hardware path could be established
  seek_failed,           ///< the container could not be positioned and re-opening failed
  not_seekable,          ///< the source cannot be rewound (a pipe) and the request needs an earlier
                         ///< position
  unusable,           ///< the input had to be re-opened and that failed; the next request retries
  decode_failed,      ///< the decoder produced no usable frame (corrupt data)
  conversion_failed,  ///< pixel-format conversion / scaling / hardware transfer failed
  time_out_of_range,  ///< requested time is beyond the last frame (see Options::out_of_range)
  end_of_stream,      ///< the stream ended before a frame covering the requested time appeared
  out_of_memory,      ///< AVERROR(ENOMEM)
  cancelled,          ///< async item was cancelled (cancel(), cancel_all(), or generator destroyed)
  internal,     ///< should-not-happen (e.g. worker thread failed to start)
};

/// A failure. `message` always names the failing operation and, where applicable, the
/// libav error string; `av_error` carries the raw negative AVERROR value (0 when not from libav).
struct Error {
  ErrorCode code{ErrorCode::internal};
  int av_error{0};
  std::string message = {};
};

[[nodiscard]] constexpr std::string_view to_string(ErrorCode c) noexcept {
  switch (c) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::invalid_state:
      return "invalid_state";
    case ErrorCode::file_not_found:
      return "file_not_found";
    case ErrorCode::open_failed:
      return "open_failed";
    case ErrorCode::unsupported_format:
      return "unsupported_format";
    case ErrorCode::no_video_stream:
      return "no_video_stream";
    case ErrorCode::decoder_not_found:
      return "decoder_not_found";
    case ErrorCode::decoder_open_failed:
      return "decoder_open_failed";
    case ErrorCode::hardware_unavailable:
      return "hardware_unavailable";
    case ErrorCode::seek_failed:
      return "seek_failed";
    case ErrorCode::not_seekable:
      return "not_seekable";
    case ErrorCode::unusable:
      return "unusable";
    case ErrorCode::decode_failed:
      return "decode_failed";
    case ErrorCode::conversion_failed:
      return "conversion_failed";
    case ErrorCode::time_out_of_range:
      return "time_out_of_range";
    case ErrorCode::end_of_stream:
      return "end_of_stream";
    case ErrorCode::out_of_memory:
      return "out_of_memory";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::internal:
      return "internal";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(const Error& e) {
  std::string s{to_string(e.code)};
  if (!e.message.empty()) {
    s += ": ";
    s += e.message;
  }
  if (e.av_error != 0) {
    s += " [AVERROR ";
    s += std::to_string(e.av_error);
    s += "]";
  }
  return s;
}

inline std::ostream& operator<<(std::ostream& os, ErrorCode c) {
  return os << to_string(c);
}
inline std::ostream& operator<<(std::ostream& os, const Error& e) {
  return os << to_string(e);
}

namespace detail {
/// The category behind stills::error_category(). One object per process: the address is the
/// identity std::error_code compares by, and every namespace-scope entity here is inline.
class ErrorCategory : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "stills"; }
  [[nodiscard]] std::string message(int value) const override {
    return std::string{to_string(static_cast<ErrorCode>(value))};
  }
};
}  // namespace detail

/// Lets an ErrorCode travel as a std::error_code, for consumers whose own interfaces speak that
/// language. Error::code is the authoritative value; this is a bridge, not a replacement.
[[nodiscard]] inline const std::error_category& error_category() noexcept {
  static const detail::ErrorCategory category;
  return category;
}

[[nodiscard]] inline std::error_code make_error_code(ErrorCode c) noexcept {
  return std::error_code{static_cast<int>(c), error_category()};
}

}  // namespace stills

template <>
struct std::is_error_code_enum<stills::ErrorCode> : std::true_type {};

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
template <>
struct std::formatter<stills::Error> : std::formatter<std::string> {
  template <class Ctx>
  auto format(const stills::Error& e, Ctx& ctx) const {
    return std::formatter<std::string>::format(stills::to_string(e), ctx);
  }
};
template <>
struct std::formatter<stills::ErrorCode> : std::formatter<std::string_view> {
  template <class Ctx>
  auto format(stills::ErrorCode c, Ctx& ctx) const {
    return std::formatter<std::string_view>::format(stills::to_string(c), ctx);
  }
};
#endif

#pragma once
// stills/stills_Error.h — the single error type used across the public API. No FFmpeg dependency.

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <version>

#include "stills/detail/stills_Config.h"

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#include <format>
#endif

namespace stills
{

/// What went wrong, expressed in terms of what the caller can do about it.
enum class ErrorCode : std::uint8_t
{
    invalidArgument,     ///< non-finite/negative time, bad Options, bad stream index, ...
    invalidState,        ///< the object cannot do this: moved from, destroyed, or the wrong kind
    fileNotFound,        ///< the source does not exist (AVERROR(ENOENT))
    openFailed,          ///< avformat_open_input failed for another reason (permissions, protocol, ...)
    unsupportedFormat,   ///< not a media container / stream info could not be read
    noVideoStream,       ///< no video stream (audio-only asset, or only cover art)
    decoderNotFound,     ///< this FFmpeg build has no decoder for the codec
    decoderOpenFailed,   ///< avcodec_open2 failed
    hardwareUnavailable, ///< requireHardware was set and no hardware path could be established
    seekFailed,          ///< the container could not be positioned and re-opening failed
    notSeekable,         ///< the source cannot be rewound (a pipe) and the request needs an earlier
                         ///< position
    unusable,            ///< the input had to be re-opened and that failed; the next request retries
    decodeFailed,        ///< the decoder produced no usable frame (corrupt data)
    conversionFailed,    ///< pixel-format conversion / scaling / hardware transfer failed
    timeOutOfRange,      ///< requested time is beyond the last frame (see Options::outOfRange)
    endOfStream,         ///< the stream ended before a frame covering the requested time appeared
    outOfMemory,         ///< AVERROR(ENOMEM)
    cancelled,           ///< async item was cancelled (cancel(), cancelAll(), or generator destroyed)
    internal,            ///< should-not-happen (e.g. worker thread failed to start)
};

/// A failure. `message` always names the failing operation and, where applicable, the
/// libav error string; `avError` carries the raw negative AVERROR value (0 when not from libav).
struct Error
{
    ErrorCode code{ ErrorCode::internal };
    int avError{ 0 };
    std::string message = {};
};

[[nodiscard]] constexpr std::string_view toString (ErrorCode c) noexcept
{
    switch (c)
    {
    case ErrorCode::invalidArgument:
        return "invalidArgument";
    case ErrorCode::invalidState:
        return "invalidState";
    case ErrorCode::fileNotFound:
        return "fileNotFound";
    case ErrorCode::openFailed:
        return "openFailed";
    case ErrorCode::unsupportedFormat:
        return "unsupportedFormat";
    case ErrorCode::noVideoStream:
        return "noVideoStream";
    case ErrorCode::decoderNotFound:
        return "decoderNotFound";
    case ErrorCode::decoderOpenFailed:
        return "decoderOpenFailed";
    case ErrorCode::hardwareUnavailable:
        return "hardwareUnavailable";
    case ErrorCode::seekFailed:
        return "seekFailed";
    case ErrorCode::notSeekable:
        return "notSeekable";
    case ErrorCode::unusable:
        return "unusable";
    case ErrorCode::decodeFailed:
        return "decodeFailed";
    case ErrorCode::conversionFailed:
        return "conversionFailed";
    case ErrorCode::timeOutOfRange:
        return "timeOutOfRange";
    case ErrorCode::endOfStream:
        return "endOfStream";
    case ErrorCode::outOfMemory:
        return "outOfMemory";
    case ErrorCode::cancelled:
        return "cancelled";
    case ErrorCode::internal:
        return "internal";
    }

    return "unknown";
}

[[nodiscard]] inline std::string toString (const Error& e)
{
    std::string s{ toString (e.code) };

    if (! e.message.empty())
    {
        s += ": ";
        s += e.message;
    }

    if (e.avError != 0)
    {
        s += " [AVERROR ";
        s += std::to_string (e.avError);
        s += "]";
    }

    return s;
}

inline std::ostream& operator<< (std::ostream& os, ErrorCode c)
{
    return os << toString (c);
}

inline std::ostream& operator<< (std::ostream& os, const Error& e)
{
    return os << toString (e);
}

namespace detail
{
// The category behind stills::getErrorCategory(). One object per process: the address is the
// identity std::error_code compares by, and every namespace-scope entity here is inline.
class ErrorCategory : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "stills"; }
    [[nodiscard]] std::string message (int value) const override
    {
        return std::string{ toString (static_cast<ErrorCode> (value)) };
    }
};
} // namespace detail

/// Lets an ErrorCode travel as a std::error_code, for consumers whose own interfaces speak that
/// language. Error::code is the authoritative value; this is a bridge, not a replacement.
[[nodiscard]] inline const std::error_category& getErrorCategory() noexcept
{
    static const detail::ErrorCategory category;
    return category;
}

[[nodiscard]] inline std::error_code make_error_code (ErrorCode c) noexcept
{
    return std::error_code{ static_cast<int> (c), getErrorCategory() };
}

} // namespace stills

template <>
struct std::is_error_code_enum<stills::ErrorCode> : std::true_type
{
};

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
template <>
struct std::formatter<stills::Error> : std::formatter<std::string>
{
    template <class Ctx>
    auto format (const stills::Error& e, Ctx& ctx) const
    {
        return std::formatter<std::string>::format (stills::toString (e), ctx);
    }
};

template <>
struct std::formatter<stills::ErrorCode> : std::formatter<std::string_view>
{
    template <class Ctx>
    auto format (stills::ErrorCode c, Ctx& ctx) const
    {
        return std::formatter<std::string_view>::format (stills::toString (c), ctx);
    }
};
#endif

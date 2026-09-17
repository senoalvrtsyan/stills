#pragma once
// stills/detail/stills_FFmpeg.h — the ONLY place libav headers are included. Provides the RAII
// deleters, C++-safe replacements for C-only macros, error mapping and enum mapping helpers.

#include <cerrno>
#include <cstddef>
#include <cstdint> // must precede libavutil (defines __STDC_CONSTANT_MACROS consumers)
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <version>

#include "stills/detail/stills_Config.h"

// libav's headers are C, and a consumer compiling with this library's own recommended warning set
// but with FFmpeg on a plain -I path (Homebrew on Apple silicon; any prefix the compiler does not
// treat as a system directory) would otherwise see those warnings — as errors — from inside
// libavutil/common.h, in a header they do not own. Suppressed only across the extern "C" block.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
// The other six exist in both compilers; -Wuseless-cast is GCC's alone, and Clang -- which defines
// __GNUC__ -- rejects the pragma naming it under -Werror (-Wunknown-warning-option).
#if ! defined(__clang__)
#pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
#endif
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
}

// FFmpeg 6.1 introduced AVFrame::duration/AV_FRAME_FLAG_KEY, AVCodecParameters::coded_side_data and
// av_packet_side_data_get; 6.0 shares the major versions but lacks them, so check the minors too.
static_assert (LIBAVCODEC_VERSION_INT >= AV_VERSION_INT (60, 31, 0),
               "stills requires FFmpeg 6.1 or newer (libavcodec >= 60.31)");
static_assert (LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT (60, 16, 0),
               "stills requires FFmpeg 6.1 or newer (libavformat >= 60.16)");
static_assert (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT (58, 29, 0),
               "stills requires FFmpeg 6.1 or newer (libavutil >= 58.29)");

#include "stills/stills_Error.h"
#include "stills/stills_Options.h"
#include "stills/stills_PixelFormat.h"
#include "stills/stills_Time.h"

namespace stills::detail
{

// libav constants that are C-cast macros, as typed values.
namespace k
{
inline constexpr std::int64_t noPts = AV_NOPTS_VALUE;
inline constexpr int eof = AVERROR_EOF;
inline constexpr int eagain = AVERROR (EAGAIN);
inline constexpr int enomem = AVERROR (ENOMEM);
inline constexpr int enoent = AVERROR (ENOENT);
inline constexpr int einval = AVERROR (EINVAL);
inline constexpr int invalidData = AVERROR_INVALIDDATA;
inline constexpr int decoderNotFound = AVERROR_DECODER_NOT_FOUND;
inline constexpr int streamNotFound = AVERROR_STREAM_NOT_FOUND;
inline constexpr int exitRequested = AVERROR_EXIT;
inline constexpr AVRational timeBaseQ{ 1, AV_TIME_BASE };
} // namespace k
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static_assert (k::noPts == std::numeric_limits<std::int64_t>::min());

/// Deleter for libav allocations. Handles both `void f(T**)` (avformat_close_input,
/// avcodec_free_context, av_frame_free, ...) and `void f(T*)` (sws_freeContext) shapes.
template <auto Fn>
struct AvDeleter
{
    template <class T>
    void operator() (T* p) const noexcept
    {
        if constexpr (std::is_invocable_v<decltype (Fn), T**>)
        {
            Fn (&p); // libav nulls our local copy; unique_ptr has already relinquished ownership
        }
        else
        {
            Fn (p);
        }
    }
};

template <class T, auto Fn>
using avPtr = std::unique_ptr<T, AvDeleter<Fn>>;

using FormatCtxPtr = avPtr<AVFormatContext, avformat_close_input>;
using CodecCtxPtr = avPtr<AVCodecContext, avcodec_free_context>;
using FramePtr = avPtr<AVFrame, av_frame_free>;
using PacketPtr = avPtr<AVPacket, av_packet_free>;
using BufferRefPtr = avPtr<AVBufferRef, av_buffer_unref>;
using SwsCtxPtr = avPtr<SwsContext, sws_freeContext>;
using DictPtr = avPtr<AVDictionary, av_dict_free>;

[[nodiscard]] inline std::string avErrorString (int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE]{};

    if (av_strerror (err, buf, sizeof buf) < 0) return "unknown error " + std::to_string (err);
    return std::string{ buf };
}

/// Builds an Error from a libav return value with context ("avformat_open_input(\"x.mp4\")").
[[nodiscard]] inline Error makeError (ErrorCode code, int avErr, std::string context)
{
    if (avErr < 0)
    {
        context += ": ";
        context += avErrorString (avErr);
    }

    return Error{ code, avErr < 0 ? avErr : 0, std::move (context) };
}

[[nodiscard]] inline Error makeError (ErrorCode code, std::string message)
{
    return Error{ code, 0, std::move (message) };
}

[[nodiscard]] inline std::unexpected<Error> fail (ErrorCode code, int avErr, std::string context)
{
    return std::unexpected (makeError (code, avErr, std::move (context)));
}

[[nodiscard]] inline std::unexpected<Error> fail (ErrorCode code, std::string message)
{
    return std::unexpected (makeError (code, std::move (message)));
}

/// Maps a generic libav return code to the closest ErrorCode when no better context exists.
[[nodiscard]] inline ErrorCode classify (int avErr, ErrorCode fallback) noexcept
{
    if (avErr == k::enomem) return ErrorCode::outOfMemory;
    if (avErr == k::enoent) return ErrorCode::fileNotFound;
    if (avErr == k::decoderNotFound) return ErrorCode::decoderNotFound;
    if (avErr == k::streamNotFound) return ErrorCode::noVideoStream;
    return fallback;
}

[[nodiscard]] constexpr AVRational toAv (Rational r) noexcept
{
    return AVRational{ r.num, r.den };
}

[[nodiscard]] constexpr Rational fromAv (AVRational r) noexcept
{
    return Rational{ r.num, r.den };
}

[[nodiscard]] constexpr AVPixelFormat toAv (PixelFormat f) noexcept
{
    switch (f)
    {
    case PixelFormat::rgba:
        return AV_PIX_FMT_RGBA;
    case PixelFormat::bgra:
        return AV_PIX_FMT_BGRA;
    case PixelFormat::argb:
        return AV_PIX_FMT_ARGB;
    case PixelFormat::abgr:
        return AV_PIX_FMT_ABGR;
    case PixelFormat::rgb0:
        return AV_PIX_FMT_RGB0;
    case PixelFormat::bgr0:
        return AV_PIX_FMT_BGR0;
    case PixelFormat::rgb24:
        return AV_PIX_FMT_RGB24;
    case PixelFormat::bgr24:
        return AV_PIX_FMT_BGR24;
    case PixelFormat::gray8:
        return AV_PIX_FMT_GRAY8;
    case PixelFormat::nv12:
        return AV_PIX_FMT_NV12;
    case PixelFormat::yuv420p:
        return AV_PIX_FMT_YUV420P;
    case PixelFormat::p010:
        return AV_PIX_FMT_P010LE;
    case PixelFormat::rgba64:
        return AV_PIX_FMT_RGBA64LE;
    }

    return AV_PIX_FMT_NONE;
}

[[nodiscard]] constexpr std::optional<PixelFormat> fromAv (AVPixelFormat f) noexcept
{
    switch (f)
    {
    case AV_PIX_FMT_RGBA:
        return PixelFormat::rgba;
    case AV_PIX_FMT_BGRA:
        return PixelFormat::bgra;
    case AV_PIX_FMT_ARGB:
        return PixelFormat::argb;
    case AV_PIX_FMT_ABGR:
        return PixelFormat::abgr;
    case AV_PIX_FMT_RGB0:
        return PixelFormat::rgb0;
    case AV_PIX_FMT_BGR0:
        return PixelFormat::bgr0;
    case AV_PIX_FMT_RGB24:
        return PixelFormat::rgb24;
    case AV_PIX_FMT_BGR24:
        return PixelFormat::bgr24;
    case AV_PIX_FMT_GRAY8:
        return PixelFormat::gray8;
    case AV_PIX_FMT_NV12:
        return PixelFormat::nv12;
    case AV_PIX_FMT_YUV420P:
        return PixelFormat::yuv420p;
    case AV_PIX_FMT_P010LE:
        return PixelFormat::p010;
    case AV_PIX_FMT_RGBA64LE:
        return PixelFormat::rgba64;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr ColorRange fromAv (AVColorRange r) noexcept
{
    switch (r)
    {
    case AVCOL_RANGE_MPEG:
        return ColorRange::limited;
    case AVCOL_RANGE_JPEG:
        return ColorRange::full;
    default:
        return ColorRange::unspecified;
    }
}

/// Resolves our device enum to libav's at runtime by name; NONE if this build lacks the type.
[[nodiscard]] inline AVHWDeviceType toAv (HardwareDeviceType t) noexcept
{
    return av_hwdevice_find_type_by_name (std::string{ toString (t) }.c_str());
}

[[nodiscard]] inline std::optional<HardwareDeviceType> fromAv (AVHWDeviceType t) noexcept
{
    const char* name = av_hwdevice_get_type_name (t);

    if (name == nullptr) return std::nullopt;
    const std::string_view n{ name };
    constexpr HardwareDeviceType all[] = { HardwareDeviceType::cuda,         HardwareDeviceType::vaapi,
                                           HardwareDeviceType::videotoolbox, HardwareDeviceType::d3d11va,
                                           HardwareDeviceType::dxva2,        HardwareDeviceType::qsv,
                                           HardwareDeviceType::vulkan,       HardwareDeviceType::vdpau,
                                           HardwareDeviceType::drm,          HardwareDeviceType::opencl,
                                           HardwareDeviceType::mediacodec };

    for (auto h : all)
    {
        if (toString (h) == n) return h;
    }

    return std::nullopt;
}

[[nodiscard]] constexpr int toSwsFlags (Scaler s) noexcept
{
    switch (s)
    {
    case Scaler::fastBilinear:
        return SWS_FAST_BILINEAR;
    case Scaler::bilinear:
        return SWS_BILINEAR;
    case Scaler::bicubic:
        return SWS_BICUBIC;
    case Scaler::area:
        return SWS_AREA;
    case Scaler::lanczos:
        return SWS_LANCZOS;
    }

    return SWS_BICUBIC;
}

/// Allocation helpers returning expected so callers never see a null libav pointer.
[[nodiscard]] inline std::expected<FramePtr, Error> makeFrame()
{
    FramePtr f{ av_frame_alloc() };

    if (! f) return fail (ErrorCode::outOfMemory, "av_frame_alloc");
    return f;
}

[[nodiscard]] inline std::expected<PacketPtr, Error> makePacket()
{
    PacketPtr p{ av_packet_alloc() };

    if (! p) return fail (ErrorCode::outOfMemory, "av_packet_alloc");
    return p;
}

} // namespace stills::detail

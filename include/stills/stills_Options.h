#pragma once
// stills/stills_Options.h — generator configuration. No FFmpeg dependency.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stills/detail/stills_Config.h"
#include "stills/stills_Error.h"
#include "stills/stills_Geometry.h"
#include "stills/stills_PixelFormat.h"
#include "stills/stills_Time.h"

namespace stills
{

/// Whether hardware decoding is attempted, and what happens when it is not available.
///
/// Every seek flushes the decoder and every returned frame is downloaded, so hardware only pays for
/// the more expensive codecs at larger sizes: HEVC/AV1/VP9/VVC from 720p up, with multi-threaded
/// software winning H.264 at every size. Any policy that tries hardware also creates the device and
/// decodes a probe frame inside open(), which is markedly slower than a software open — much more
/// so with a discrete GPU than with an integrated one.
enum class HardwarePolicy : std::uint8_t
{
    automatic,       ///< hardware only where it is expected to be faster (default): HEVC/AV1/VP9/VVC
                     ///< at 720p and above; software otherwise. Falls back to software.
    preferHardware,  ///< always try hardware, transparently fall back to software
    softwareOnly,    ///< never touch a hardware device
    requireHardware, ///< fail open() with hardwareUnavailable when no hardware path works
};

[[nodiscard]] constexpr std::string_view toString (HardwarePolicy p) noexcept
{
    switch (p)
    {
    case HardwarePolicy::automatic:
        return "automatic";
    case HardwarePolicy::preferHardware:
        return "preferHardware";
    case HardwarePolicy::softwareOnly:
        return "softwareOnly";
    case HardwarePolicy::requireHardware:
        return "requireHardware";
    }

    return "unknown";
}

/// Hardware device families known to FFmpeg. Resolved by name at runtime, so an FFmpeg build
/// without a given type simply reports it as unavailable.
enum class HardwareDeviceType : std::uint8_t
{
    cuda,
    vaapi,
    videotoolbox,
    d3d11va,
    dxva2,
    qsv,
    vulkan,
    vdpau,
    drm,
    opencl,
    mediacodec,
};

[[nodiscard]] constexpr std::string_view toString (HardwareDeviceType t) noexcept
{
    switch (t)
    {
    case HardwareDeviceType::cuda:
        return "cuda";
    case HardwareDeviceType::vaapi:
        return "vaapi";
    case HardwareDeviceType::videotoolbox:
        return "videotoolbox";
    case HardwareDeviceType::d3d11va:
        return "d3d11va";
    case HardwareDeviceType::dxva2:
        return "dxva2";
    case HardwareDeviceType::qsv:
        return "qsv";
    case HardwareDeviceType::vulkan:
        return "vulkan";
    case HardwareDeviceType::vdpau:
        return "vdpau";
    case HardwareDeviceType::drm:
        return "drm";
    case HardwareDeviceType::opencl:
        return "opencl";
    case HardwareDeviceType::mediacodec:
        return "mediacodec";
    }

    return "unknown";
}

/// Scaling filter used when the output is smaller than the source.
enum class Scaler : std::uint8_t
{
    fastBilinear,
    bilinear,
    bicubic,
    area,
    lanczos
};

[[nodiscard]] constexpr std::string_view toString (Scaler s) noexcept
{
    switch (s)
    {
    case Scaler::fastBilinear:
        return "fastBilinear";
    case Scaler::bilinear:
        return "bilinear";
    case Scaler::bicubic:
        return "bicubic";
    case Scaler::area:
        return "area";
    case Scaler::lanczos:
        return "lanczos";
    }

    return "unknown";
}

/// What to do with a requested time beyond the last frame.
enum class OutOfRangePolicy : std::uint8_t
{
    error,            ///< return ErrorCode::timeOutOfRange (default; surfaces caller bugs)
    clampToLastFrame, ///< return the last frame with Image::wasClamped() == true
};

[[nodiscard]] constexpr std::string_view toString (OutOfRangePolicy p) noexcept
{
    switch (p)
    {
    case OutOfRangePolicy::error:
        return "error";
    case OutOfRangePolicy::clampToLastFrame:
        return "clampToLastFrame";
    }

    return "unknown";
}

/// How hardware decoding is attempted, and on what. Grouped because the three only matter together.
struct HardwareOptions
{
    HardwarePolicy policy = HardwarePolicy::automatic;
    /// Restrict hardware decoding to one device family. nullopt = try every family the codec
    /// supports.
    std::optional<HardwareDeviceType> deviceType = std::nullopt;
    /// Device string passed to av_hwdevice_ctx_create (e.g. "/dev/dri/renderD128", "0"). Empty =
    /// default. Requires deviceType (a device string is family-specific).
    std::string device = {};
};

/// Per-request overrides of Options. They travel with the request, so they are thread-safe by
/// construction: the same generator serves a batch of `Tolerance::any()` thumbnails and an exact
/// full-size still, without a second decoder. The pixel format stays fixed (it determines the
/// converter).
struct RequestOptions
{
    std::optional<Tolerance> tolerance = std::nullopt; ///< default: Options::tolerance
    /// Output box for this request. nullopt (default) = the generator's Options::maximumSize;
    /// Size{} (both dimensions 0) = native size even when the generator has a box; any other value is
    /// a fit-within box like Options::maximumSize.
    std::optional<Size> maximumSize = std::nullopt;

    /// Checks the overrides against the generator's pixel format. imageAt()/generateImages() report
    /// a failure as invalidArgument (per item, for batches).
    [[nodiscard]] std::expected<void, Error> validate (PixelFormat format) const
    {
        const auto bad = [] (std::string message)
        { return std::unexpected (Error{ ErrorCode::invalidArgument, 0, std::move (message) }); };

        if (maximumSize)
        {
            if (maximumSize->width < 0 || maximumSize->height < 0)
                return bad ("RequestOptions::maximumSize must not be negative");

            if (hasChromaSubsampling (format)
                && ((maximumSize->width > 0 && maximumSize->width < 2)
                    || (maximumSize->height > 0 && maximumSize->height < 2)))
            {
                return bad ("RequestOptions::maximumSize must be at least 2x2 for chroma-subsampled output "
                            "formats");
            }
        }

        if (tolerance)
        {
            for (const Time* t : { &tolerance->before, &tolerance->after })
            {
                const bool ok = t->isPositiveInfinity() || (t->isFinite() && ! t->isNegative());

                if (! ok) return bad ("RequestOptions::tolerance must be finite and non-negative, or +inf");
            }
        }

        return {};
    }
};

/// Configuration fixed at AssetImageGenerator::open(). Immutable afterwards.
struct Options
{
    /// Upper bounds validate() enforces, so a typo (INT_MAX, SIZE_MAX) is an invalidArgument at
    /// open() rather than an out-of-memory from libavcodec several calls later.
    static constexpr int maxDecoderThreads = 1024;

    /// Fit-within box, aspect ratio preserved, never upscaled. nullopt = native size. Either
    /// dimension may be 0 (unconstrained). Chroma-subsampled outputs (nv12, yuv420p) need at least 2.
    std::optional<Size> maximumSize = std::nullopt;
    PixelFormat pixelFormat = PixelFormat::rgba;
    /// Default is frame-accurate (Apple's default is infinite; see README "Deviations").
    Tolerance tolerance = Tolerance::exact();
    /// Honour the container's display matrix — rotation *and* mirroring (phone footage). Apple's
    /// default is false; ours is true because upright output is what nearly every consumer wants.
    bool applyPreferredTrackTransform = true;
    /// Resample anamorphic content to square pixels.
    bool applySampleAspectRatio = true;
    Scaler scaler = Scaler::bicubic;
    OutOfRangePolicy outOfRange = OutOfRangePolicy::error;

    HardwareOptions hardware = {};

    /// Refuse a video stream whose coded frame is larger than this many pixels (width * height).
    /// nullopt (default) = no limit. A container costs a few hundred bytes to *declare* an enormous
    /// frame and open() allocates for it, because the decoder is set up and the first frame probed
    /// there: a tiny file can cost gigabytes of resident memory inside open() alone. The limit is
    /// checked against the container's declaration before the decoder is built, and handed to
    /// libavcodec (AVCodecContext::max_pixels) so a mid-stream resolution change is caught too.
    /// Exceeding it fails with unsupportedFormat naming the size.
    /// 8294400 is 4K (3840x2160).
    std::optional<std::int64_t> maxInputPixels = std::nullopt;

    /// Explicit video stream index (>= 0). nullopt = av_find_best_stream.
    std::optional<int> videoStreamIndex = std::nullopt;
    /// Frame-decoder threads. 0 (default) lets libavcodec pick (one frame thread per hardware thread,
    /// capped at 16), which is the fastest single request. A smaller count bounds memory and CPU when
    /// many generators are open, at the cost of per-request latency; 2-4 was the useful range on the
    /// author's machine — a starting point, not a portable truth.
    ///
    /// A hardware decoder always uses one thread (hwaccel plus frame threads buys nothing), so this
    /// value is ignored for it.
    int decoderThreads = 0;
    /// Treat attached pictures (cover art) as a video stream when nothing else is available.
    bool allowAttachedPictures = false;
    /// Demuxer/protocol options forwarded to avformat_open_input as an AVDictionary, e.g.
    /// {"probesize", "1000000"}, {"rw_timeout", "5000000"}, {"reconnect", "1"} for network sources.
    ///
    /// **Untrusted input.** The source string reaches every protocol this FFmpeg build has, so a
    /// path-prefix check on it proves nothing: `concat:a|/etc/passwd`,
    /// `subfile,,start,0,end,0,,:/etc/passwd` and a playlist referencing arbitrary files all open.
    /// Two of libavformat's own keys close that off, and both belong here. `protocol_whitelist`
    /// confines the source string; `format_whitelist` confines the container the byte stream is
    /// demuxed as. libavformat applies both to nested opens too, so a local playlist cannot reach
    /// the network under them. For media whose content and name come from outside:
    ///
    ///     o.demuxerOptions = {{"protocol_whitelist", "file"},
    ///                          {"format_whitelist", "mov,mp4,m4a,matroska,webm"}};
    ///     o.maxInputPixels = std::int64_t{3840} * 2160;  // refuse an 8K header before decoding
    ///
    /// Widen the protocol list for remote media ("file,http,https,tcp,tls"). None of this makes a
    /// decoder safe against a malicious *bitstream* — that is libavcodec's problem; it removes the
    /// ways a file *name* or a header *field* alone can reach other files or exhaust memory.
    ///
    /// Keys libavformat does not consume are reported: a misspelled one fails open() with
    /// invalidArgument rather than being silently ignored.
    std::vector<std::pair<std::string, std::string>> demuxerOptions = {};

    /// Checks every option that can be checked without opening the asset. open() calls this and
    /// reports the first problem as ErrorCode::invalidArgument.
    [[nodiscard]] std::expected<void, Error> validate() const
    {
        const auto bad = [] (std::string message)
        { return std::unexpected (Error{ ErrorCode::invalidArgument, 0, std::move (message) }); };

        if (maximumSize)
        {
            if (maximumSize->width < 0 || maximumSize->height < 0) return bad ("maximumSize must not be negative");
            if (hasChromaSubsampling (pixelFormat)
                && ((maximumSize->width > 0 && maximumSize->width < 2)
                    || (maximumSize->height > 0 && maximumSize->height < 2)))
            {
                return bad ("maximumSize must be at least 2x2 for chroma-subsampled output formats");
            }
        }

        if (decoderThreads < 0) return bad ("decoderThreads must not be negative");
        if (decoderThreads > maxDecoderThreads)
        {
            return bad ("decoderThreads must be at most " + std::to_string (maxDecoderThreads)
                        + " (0 = let libavcodec decide)");
        }

        if (maxInputPixels && *maxInputPixels < 1)
            return bad ("maxInputPixels must be at least 1 (use nullopt for no limit)");

        for (const Time* t : { &tolerance.before, &tolerance.after })
        {
            const bool ok = t->isPositiveInfinity() || (t->isFinite() && ! t->isNegative());

            if (! ok) return bad ("tolerance must be finite and non-negative, or +inf");
        }

        if (videoStreamIndex && *videoStreamIndex < 0) return bad ("videoStreamIndex must not be negative");
        if (hardware.policy == HardwarePolicy::softwareOnly && (hardware.deviceType || ! hardware.device.empty()))
        {
            return bad ("hardware.deviceType/device have no effect with HardwarePolicy::softwareOnly");
        }

        if (! hardware.device.empty() && ! hardware.deviceType)
        {
            return bad ("hardware.device requires hardware.deviceType (device strings are family-specific)");
        }

        for (const auto& [key, value] : demuxerOptions)
        {
            if (key.empty()) return bad ("demuxerOptions keys must not be empty");
            (void)value;
        }

        return {};
    }
};

} // namespace stills

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
STILLS_DEFINE_ENUM_FORMATTER (stills::HardwareDeviceType);
STILLS_DEFINE_ENUM_FORMATTER (stills::HardwarePolicy);
STILLS_DEFINE_ENUM_FORMATTER (stills::Scaler);
STILLS_DEFINE_ENUM_FORMATTER (stills::OutOfRangePolicy);
#endif

#pragma once
// stills/options.hpp — generator configuration. No FFmpeg dependency.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stills/detail/config.hpp"
#include "stills/error.hpp"
#include "stills/geometry.hpp"
#include "stills/pixel_format.hpp"
#include "stills/time.hpp"

namespace stills {

/// Whether hardware decoding is attempted, and what happens when it is not available.
///
/// Every seek flushes the decoder and every returned frame is downloaded, so hardware only pays for
/// the more expensive codecs at larger sizes: HEVC/AV1/VP9/VVC from 720p up, with multi-threaded
/// software winning H.264 at every size. Any policy that tries hardware also creates the device and
/// decodes a probe frame inside open(), which is markedly slower than a software open — much more
/// so with a discrete GPU than with an integrated one.
enum class HardwarePolicy : std::uint8_t {
  automatic,        ///< hardware only where it is expected to be faster (default): HEVC/AV1/VP9/VVC
                    ///< at 720p and above; software otherwise. Falls back to software.
  prefer_hardware,  ///< always try hardware, transparently fall back to software
  software_only,    ///< never touch a hardware device
  require_hardware,  ///< fail open() with hardware_unavailable when no hardware path works
};

[[nodiscard]] constexpr std::string_view to_string(HardwarePolicy p) noexcept {
  switch (p) {
    case HardwarePolicy::automatic:
      return "automatic";
    case HardwarePolicy::prefer_hardware:
      return "prefer_hardware";
    case HardwarePolicy::software_only:
      return "software_only";
    case HardwarePolicy::require_hardware:
      return "require_hardware";
  }
  return "unknown";
}

/// Hardware device families known to FFmpeg. Resolved by name at runtime, so an FFmpeg build
/// without a given type simply reports it as unavailable.
enum class HardwareDeviceType : std::uint8_t {
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

[[nodiscard]] constexpr std::string_view to_string(HardwareDeviceType t) noexcept {
  switch (t) {
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
enum class Scaler : std::uint8_t { fast_bilinear, bilinear, bicubic, area, lanczos };

[[nodiscard]] constexpr std::string_view to_string(Scaler s) noexcept {
  switch (s) {
    case Scaler::fast_bilinear:
      return "fast_bilinear";
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
enum class OutOfRangePolicy : std::uint8_t {
  error,                ///< return ErrorCode::time_out_of_range (default; surfaces caller bugs)
  clamp_to_last_frame,  ///< return the last frame with Image::was_clamped() == true
};

[[nodiscard]] constexpr std::string_view to_string(OutOfRangePolicy p) noexcept {
  switch (p) {
    case OutOfRangePolicy::error:
      return "error";
    case OutOfRangePolicy::clamp_to_last_frame:
      return "clamp_to_last_frame";
  }
  return "unknown";
}

/// How hardware decoding is attempted, and on what. Grouped because the three only matter together.
struct HardwareOptions {
  HardwarePolicy policy = HardwarePolicy::automatic;
  /// Restrict hardware decoding to one device family. nullopt = try every family the codec
  /// supports.
  std::optional<HardwareDeviceType> device_type = std::nullopt;
  /// Device string passed to av_hwdevice_ctx_create (e.g. "/dev/dri/renderD128", "0"). Empty =
  /// default. Requires device_type (a device string is family-specific).
  std::string device = {};
};

/// Per-request overrides of Options. They travel with the request, so they are thread-safe by
/// construction: the same generator serves a batch of `Tolerance::any()` thumbnails and an exact
/// full-size still, without a second decoder. The pixel format stays fixed (it determines the
/// converter).
struct RequestOptions {
  std::optional<Tolerance> tolerance = std::nullopt;  ///< default: Options::tolerance
  /// Output box for this request. nullopt (default) = the generator's Options::maximum_size;
  /// Size{} (both dimensions 0) = native size even when the generator has a box; any other value is
  /// a fit-within box like Options::maximum_size.
  std::optional<Size> maximum_size = std::nullopt;

  /// Checks the overrides against the generator's pixel format. image_at()/generate_images() report
  /// a failure as invalid_argument (per item, for batches).
  [[nodiscard]] std::expected<void, Error> validate(PixelFormat format) const {
    const auto bad = [](std::string message) {
      return std::unexpected(Error{ErrorCode::invalid_argument, 0, std::move(message)});
    };
    if (maximum_size) {
      if (maximum_size->width < 0 || maximum_size->height < 0)
        return bad("RequestOptions::maximum_size must not be negative");
      if (has_chroma_subsampling(format) &&
          ((maximum_size->width > 0 && maximum_size->width < 2) ||
           (maximum_size->height > 0 && maximum_size->height < 2))) {
        return bad(
            "RequestOptions::maximum_size must be at least 2x2 for chroma-subsampled output "
            "formats");
      }
    }
    if (tolerance) {
      for (const Time* t : {&tolerance->before, &tolerance->after}) {
        const bool ok = t->is_positive_infinity() || (t->is_finite() && !t->is_negative());
        if (!ok) return bad("RequestOptions::tolerance must be finite and non-negative, or +inf");
      }
    }
    return {};
  }
};

/// Configuration fixed at AssetImageGenerator::open(). Immutable afterwards.
struct Options {
  /// Upper bounds validate() enforces, so a typo (INT_MAX, SIZE_MAX) is an invalid_argument at
  /// open() rather than an out-of-memory from libavcodec several calls later.
  static constexpr int max_decoder_threads = 1024;

  /// Fit-within box, aspect ratio preserved, never upscaled. nullopt = native size. Either
  /// dimension may be 0 (unconstrained). Chroma-subsampled outputs (nv12, yuv420p) need at least 2.
  std::optional<Size> maximum_size = std::nullopt;
  PixelFormat pixel_format = PixelFormat::rgba;
  /// Default is frame-accurate (Apple's default is infinite; see README "Deviations").
  Tolerance tolerance = Tolerance::exact();
  /// Honour the container's display matrix — rotation *and* mirroring (phone footage). Apple's
  /// default is false; ours is true because upright output is what nearly every consumer wants.
  bool apply_preferred_track_transform = true;
  /// Resample anamorphic content to square pixels.
  bool apply_sample_aspect_ratio = true;
  Scaler scaler = Scaler::bicubic;
  OutOfRangePolicy out_of_range = OutOfRangePolicy::error;

  HardwareOptions hardware = {};

  /// Refuse a video stream whose coded frame is larger than this many pixels (width * height).
  /// nullopt (default) = no limit. A container costs a few hundred bytes to *declare* an enormous
  /// frame and open() allocates for it, because the decoder is set up and the first frame probed
  /// there: a tiny file can cost gigabytes of resident memory inside open() alone. The limit is
  /// checked against the container's declaration before the decoder is built, and handed to
  /// libavcodec (AVCodecContext::max_pixels) so a mid-stream resolution change is caught too.
  /// Exceeding it fails with unsupported_format naming the size.
  /// 8294400 is 4K (3840x2160).
  std::optional<std::int64_t> max_input_pixels = std::nullopt;

  /// Explicit video stream index (>= 0). nullopt = av_find_best_stream.
  std::optional<int> video_stream_index = std::nullopt;
  /// Frame-decoder threads. 0 (default) lets libavcodec pick (one frame thread per hardware thread,
  /// capped at 16), which is the fastest single request. A smaller count bounds memory and CPU when
  /// many generators are open, at the cost of per-request latency; 2-4 was the useful range on the
  /// author's machine — a starting point, not a portable truth.
  ///
  /// A hardware decoder always uses one thread (hwaccel plus frame threads buys nothing), so this
  /// value is ignored for it.
  int decoder_threads = 0;
  /// Treat attached pictures (cover art) as a video stream when nothing else is available.
  bool allow_attached_pictures = false;
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
  ///     o.demuxer_options = {{"protocol_whitelist", "file"},
  ///                          {"format_whitelist", "mov,mp4,m4a,matroska,webm"}};
  ///     o.max_input_pixels = std::int64_t{3840} * 2160;  // refuse an 8K header before decoding
  ///
  /// Widen the protocol list for remote media ("file,http,https,tcp,tls"). None of this makes a
  /// decoder safe against a malicious *bitstream* — that is libavcodec's problem; it removes the
  /// ways a file *name* or a header *field* alone can reach other files or exhaust memory.
  ///
  /// Keys libavformat does not consume are reported: a misspelled one fails open() with
  /// invalid_argument rather than being silently ignored.
  std::vector<std::pair<std::string, std::string>> demuxer_options = {};

  /// Checks every option that can be checked without opening the asset. open() calls this and
  /// reports the first problem as ErrorCode::invalid_argument.
  [[nodiscard]] std::expected<void, Error> validate() const {
    const auto bad = [](std::string message) {
      return std::unexpected(Error{ErrorCode::invalid_argument, 0, std::move(message)});
    };
    if (maximum_size) {
      if (maximum_size->width < 0 || maximum_size->height < 0)
        return bad("maximum_size must not be negative");
      if (has_chroma_subsampling(pixel_format) &&
          ((maximum_size->width > 0 && maximum_size->width < 2) ||
           (maximum_size->height > 0 && maximum_size->height < 2))) {
        return bad("maximum_size must be at least 2x2 for chroma-subsampled output formats");
      }
    }
    if (decoder_threads < 0) return bad("decoder_threads must not be negative");
    if (decoder_threads > max_decoder_threads) {
      return bad("decoder_threads must be at most " + std::to_string(max_decoder_threads) +
                 " (0 = let libavcodec decide)");
    }
    if (max_input_pixels && *max_input_pixels < 1)
      return bad("max_input_pixels must be at least 1 (use nullopt for no limit)");
    for (const Time* t : {&tolerance.before, &tolerance.after}) {
      const bool ok = t->is_positive_infinity() || (t->is_finite() && !t->is_negative());
      if (!ok) return bad("tolerance must be finite and non-negative, or +inf");
    }
    if (video_stream_index && *video_stream_index < 0)
      return bad("video_stream_index must not be negative");
    if (hardware.policy == HardwarePolicy::software_only &&
        (hardware.device_type || !hardware.device.empty())) {
      return bad("hardware.device_type/device have no effect with HardwarePolicy::software_only");
    }
    if (!hardware.device.empty() && !hardware.device_type) {
      return bad(
          "hardware.device requires hardware.device_type (device strings are family-specific)");
    }
    for (const auto& [key, value] : demuxer_options) {
      if (key.empty()) return bad("demuxer_options keys must not be empty");
      (void)value;
    }
    return {};
  }
};

}  // namespace stills

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
STILLS_DEFINE_ENUM_FORMATTER(stills::HardwareDeviceType);
STILLS_DEFINE_ENUM_FORMATTER(stills::HardwarePolicy);
STILLS_DEFINE_ENUM_FORMATTER(stills::Scaler);
STILLS_DEFINE_ENUM_FORMATTER(stills::OutOfRangePolicy);
#endif

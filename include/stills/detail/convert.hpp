#pragma once
// stills/detail/convert.hpp — output geometry (SAR, rotation, fit-within, even rule), swscale
// conversion with correct colour matrix/range, and hardware->software transfer.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

#include "stills/detail/ffmpeg.hpp"
#include "stills/detail/hooks.hpp"
#include "stills/detail/transform.hpp"
#include "stills/geometry.hpp"
#include "stills/options.hpp"
#include "stills/pixel_format.hpp"

namespace stills::detail {

/// Display size of a coded frame: sample-aspect-ratio corrected (when enabled) and rotated.
[[nodiscard]] inline Size display_size(Size coded, AVRational sar, bool apply_sar,
                                       int rotation) noexcept {
  Size d = coded;
  if (apply_sar && sar.num > 0 && sar.den > 0 && sar.num != sar.den) {
    const double w = static_cast<double>(coded.width) * sar.num / sar.den;
    d.width = std::max(1, static_cast<int>(std::lround(w)));
  }
  if (rotation == 90 || rotation == 270) d = d.transposed();
  return d;
}

/// Fits `display` into `max` (either dimension 0/absent = unconstrained) preserving aspect ratio,
/// never upscaling, and rounding down to even for chroma-subsampled formats (minimum 2x2; boxes
/// smaller than that are rejected by Options::validate).
[[nodiscard]] inline Size fit_size(Size display, std::optional<Size> max,
                                   PixelFormat fmt) noexcept {
  double scale = 1.0;
  if (max) {
    if (max->width > 0) scale = std::min(scale, static_cast<double>(max->width) / display.width);
    if (max->height > 0) scale = std::min(scale, static_cast<double>(max->height) / display.height);
  }
  Size out{std::max(1, static_cast<int>(std::lround(display.width * scale))),
           std::max(1, static_cast<int>(std::lround(display.height * scale)))};
  if (has_chroma_subsampling(fmt)) {
    out.width = std::max(2, out.width & ~1);
    out.height = std::max(2, out.height & ~1);
  }
  return out;
}

/// Resolves the sample aspect ratio the way av_guess_sample_aspect_ratio (and ffmpeg/ffplay) do:
/// the container's declaration wins, then the frame's, then the codec's; 1:1 when nothing is set.
[[nodiscard]] inline AVRational resolve_sar(AVRational container, AVRational frame,
                                            AVRational codec) noexcept {
  const auto ok = [](AVRational r) { return r.num > 0 && r.den > 0; };
  if (ok(container)) return container;
  if (ok(frame)) return frame;
  if (ok(codec)) return codec;
  return AVRational{1, 1};
}

[[nodiscard]] inline std::expected<FramePtr, Error> transfer_to_software_unpooled(
    const AVFrame& hw);

/// Converts decoded software frames to the configured output format/size.
class Converter {
 public:
  /// `strip_display_matrix`: the display transform has been applied, so the output frame must not
  /// carry the AV_FRAME_DATA_DISPLAYMATRIX side data any more (an interop consumer honouring it
  /// would rotate a second time).
  Converter(PixelFormat fmt, Scaler scaler, std::optional<Size> max, bool apply_sar, int rotation,
            bool mirror, bool strip_display_matrix) noexcept
      : fmt_(fmt),
        // SWS_ACCURATE_RND is deliberately not set: it disables swscale's fast YUV->RGB paths for
        // at most one LSB. SWS_FULL_CHR_H_INT (full chroma interpolation for RGB outputs) is free
        // on those paths.
        sws_flags_(to_sws_flags(scaler) | SWS_FULL_CHR_H_INT),
        max_(max),
        apply_sar_(apply_sar),
        rotation_(rotation),
        mirror_(mirror),
        strip_display_matrix_(strip_display_matrix) {}

  [[nodiscard]] PixelFormat pixel_format() const noexcept { return fmt_; }
  [[nodiscard]] int rotation() const noexcept { return rotation_; }
  [[nodiscard]] bool mirror() const noexcept { return mirror_; }

  /// Final (rotated) output size for a source of this coded size and SAR.
  [[nodiscard]] Size output_size(Size coded, AVRational sar) const noexcept {
    return fit_size(display_size(coded, sar, apply_sar_, rotation_), max_, fmt_);
  }

  /// Downloads a hardware frame into a pooled software frame (props copied). Pooling matters: a
  /// fresh allocation per transfer is large enough for glibc to mmap it, so every transfer pays for
  /// page faults on top of the DMA.
  [[nodiscard]] std::expected<FramePtr, Error> download(const AVFrame& hw) {
    AVPixelFormat sw_fmt = AV_PIX_FMT_NONE;
    if (hw.hw_frames_ctx != nullptr)
      sw_fmt = reinterpret_cast<const AVHWFramesContext*>(hw.hw_frames_ctx->data)->sw_format;
    if (sw_fmt == AV_PIX_FMT_NONE) return transfer_to_software_unpooled(hw);
    auto sw = pooled_frame(sw_fmt, hw.width, hw.height, "download");
    if (!sw) return std::unexpected(sw.error());
    // Through the hook (detail/hooks.hpp) so the rebuild-once path a driver fault drives can be
    // tested on the shipped code. Defaults to av_hwframe_transfer_data itself.
    if (int r = decoder_hooks().hwframe_transfer_data(sw->get(), &hw, 0); r < 0) {
      return fail(ErrorCode::conversion_failed, r, "av_hwframe_transfer_data");
    }
    if (int r = av_frame_copy_props(sw->get(), &hw); r < 0) {
      return fail(ErrorCode::conversion_failed, r, "av_frame_copy_props(transfer)");
    }
    return std::move(*sw);
  }

  /// Final (rotated) output size with a per-request box override (RequestOptions::maximum_size).
  [[nodiscard]] Size output_size(Size coded, AVRational sar,
                                 std::optional<Size> max_override) const noexcept {
    return fit_size(display_size(coded, sar, apply_sar_, rotation_),
                    max_override ? max_override : max_, fmt_);
  }

  /// Scales/converts `src` (a software frame) and applies the display transform. The SAR is
  /// resolved from the container's, the frame's and the codec's declarations in that order.
  /// Output and intermediate frames come from per-size buffer pools, so a steady stream of requests
  /// recycles a few buffers instead of allocating a full frame per image.
  [[nodiscard]] std::expected<FramePtr, Error> convert(
      const AVFrame& src, AVRational container_sar, AVRational codec_sar,
      std::optional<Size> max_override = std::nullopt) {
    const AVRational sar = resolve_sar(container_sar, src.sample_aspect_ratio, codec_sar);
    const Size coded{src.width, src.height};
    const Size rotated_target = output_size(coded, sar, max_override);
    const bool swap = rotation_ == 90 || rotation_ == 270;
    const Size target = swap ? rotated_target.transposed() : rotated_target;

    auto dst = pooled_frame(to_av(fmt_), target.width, target.height, "output");
    if (!dst) return std::unexpected(dst.error());
    if (int r = av_frame_copy_props(dst->get(), &src); r < 0) {
      return fail(ErrorCode::conversion_failed, r, "av_frame_copy_props(output)");
    }

    // swscale has no fast path from semi-planar NV12 (what hardware decoders hand back) to packed
    // RGB, so a hardware frame is re-planed into YUV420P first: that copy costs far less than the
    // slow path it avoids.
    const AVFrame* in = &src;
    FramePtr replaned;
    if (needs_replaning(static_cast<AVPixelFormat>(src.format))) {
      auto r = replane(src);
      if (!r) return std::unexpected(std::move(r.error()));
      replaned = std::move(*r);
      in = replaned.get();
    }

    const auto src_fmt = static_cast<AVPixelFormat>(in->format);
    SwsContext* sws = scaler_for(in->width, in->height, src_fmt, target.width, target.height);
    if (sws == nullptr) {
      return fail(ErrorCode::conversion_failed,
                  std::string("sws_getContext: cannot convert ") +
                      (av_get_pix_fmt_name(src_fmt) ? av_get_pix_fmt_name(src_fmt) : "?") + " to " +
                      std::string{to_string(fmt_)});
    }
    apply_colorspace(*in, dst->get(), sws);

    if (int r = sws_scale_frame(sws, dst->get(), in); r < 0) {
      return fail(ErrorCode::conversion_failed, r, "sws_scale_frame");
    }
    // Square pixels once the SAR has been resampled away; otherwise the source SAR is preserved
    // for interop consumers (transposed when the image is rotated by a quarter turn).
    if (apply_sar_) {
      (*dst)->sample_aspect_ratio = AVRational{1, 1};
    } else {
      (*dst)->sample_aspect_ratio = swap ? AVRational{sar.den, sar.num} : sar;
    }
    if (strip_display_matrix_) av_frame_remove_side_data(dst->get(), AV_FRAME_DATA_DISPLAYMATRIX);

    if (rotation_ != 0 || mirror_) {
      auto rotated =
          pooled_frame(to_av(fmt_), rotated_target.width, rotated_target.height, "transformed");
      if (!rotated) return std::unexpected(rotated.error());
      if (auto r = transform_into(**dst, *rotated->get(), fmt_, rotation_, mirror_); !r)
        return std::unexpected(r.error());
      return std::move(*rotated);
    }
    return std::move(*dst);
  }

 private:
  /// NV12/NV21 sources headed for a packed RGB or gray output take the two-step route.
  [[nodiscard]] bool needs_replaning(AVPixelFormat f) const noexcept {
    return (f == AV_PIX_FMT_NV12 || f == AV_PIX_FMT_NV21) &&
           (is_packed_rgb(fmt_) || fmt_ == PixelFormat::gray8);
  }

  /// NV12/NV21 -> YUV420P at the same size: a plane rearrangement (colour metadata preserved).
  [[nodiscard]] std::expected<FramePtr, Error> replane(const AVFrame& src) {
    auto out = pooled_frame(AV_PIX_FMT_YUV420P, src.width, src.height, "replane");
    if (!out) return std::unexpected(out.error());
    if (int r = av_frame_copy_props(out->get(), &src); r < 0) {
      return fail(ErrorCode::conversion_failed, r, "av_frame_copy_props(replane)");
    }
    SwsContext* ctx = sws_getCachedContext(
        sws_replane_.release(), src.width, src.height, static_cast<AVPixelFormat>(src.format),
        src.width, src.height, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
    sws_replane_.reset(ctx);
    if (!sws_replane_) return fail(ErrorCode::conversion_failed, "sws_getCachedContext(replane)");
    if (int r = sws_scale_frame(sws_replane_.get(), out->get(), &src); r < 0) {
      return fail(ErrorCode::conversion_failed, r, "sws_scale_frame(replane)");
    }
    return std::move(*out);
  }

  /// A scaling context for this geometry from a small most-recently-used set, so that alternating
  /// output sizes (a native-size still and a 160 px thumbnail) do not re-create one per request.
  [[nodiscard]] SwsContext* scaler_for(int sw, int sh, AVPixelFormat sfmt, int dw, int dh) {
    ++clock_;
    std::size_t victim = 0;
    for (std::size_t i = 0; i < scalers_.size(); ++i) {
      Scaler& s = scalers_[i];
      if (s.ctx && s.sw == sw && s.sh == sh && s.sfmt == sfmt && s.dw == dw && s.dh == dh) {
        s.used = clock_;
        return s.ctx.get();
      }
      if (!s.ctx || s.used < scalers_[victim].used) victim = i;
    }
    SwsCtxPtr ctx{
        sws_getContext(sw, sh, sfmt, dw, dh, to_av(fmt_), sws_flags_, nullptr, nullptr, nullptr)};
    if (!ctx) return nullptr;
    Scaler& s = scalers_[victim];
    s.sw = sw;
    s.sh = sh;
    s.sfmt = sfmt;
    s.dw = dw;
    s.dh = dh;
    s.used = clock_;
    s.ctx = std::move(ctx);
    return s.ctx.get();
  }

  /// A frame whose pixel buffer comes from a pool keyed on (format, width, height): the layout is
  /// the one av_frame_get_buffer() produces (64-byte aligned lines, 32-row padded height) so rows
  /// may be padded exactly as before. Buffers return to the pool when the Image is destroyed.
  [[nodiscard]] std::expected<FramePtr, Error> pooled_frame(AVPixelFormat fmt, int width,
                                                            int height, const char* what) {
    constexpr int align = 64;
    ++clock_;
    std::size_t slot = pools_.size();
    std::size_t victim = 0;
    for (std::size_t i = 0; i < pools_.size(); ++i) {
      if (pools_[i].pool && pools_[i].fmt == fmt && pools_[i].width == width &&
          pools_[i].height == height) {
        slot = i;
        break;
      }
      if (!pools_[i].pool || pools_[i].used < pools_[victim].used) victim = i;
    }
    if (slot == pools_.size()) {
      Pool& p = pools_[victim];
      p.pool.reset();
      p.fmt = fmt;
      p.width = width;
      p.height = height;
      if (int r = av_image_fill_linesizes(p.linesize, fmt, FFALIGN(width, align)); r < 0) {
        return fail(ErrorCode::conversion_failed, r,
                    std::string("av_image_fill_linesizes(") + what + ")");
      }
      std::array<std::ptrdiff_t, 4> ls{};
      for (int i = 0; i < 4; ++i) {
        p.linesize[i] = FFALIGN(p.linesize[i], align);
        ls[static_cast<std::size_t>(i)] = p.linesize[i];
      }
      p.padded_height = FFALIGN(height, 32);
      std::array<std::size_t, 4> sizes{};
      if (int r = av_image_fill_plane_sizes(sizes.data(), fmt, p.padded_height, ls.data()); r < 0) {
        return fail(ErrorCode::conversion_failed, r,
                    std::string("av_image_fill_plane_sizes(") + what + ")");
      }
      std::size_t total = 0;
      for (std::size_t sz : sizes) total += sz;
      total += 4 * 16 + align;  // plane padding, as av_frame_get_buffer adds
      p.pool.reset(av_buffer_pool_init(total, nullptr));
      if (!p.pool)
        return fail(ErrorCode::out_of_memory, std::string("av_buffer_pool_init(") + what + ")");
      slot = victim;
    }
    Pool& pool = pools_[slot];
    pool.used = clock_;
    auto frame = make_frame();
    if (!frame) return std::unexpected(frame.error());
    AVBufferRef* buf = av_buffer_pool_get(pool.pool.get());
    if (buf == nullptr)
      return fail(ErrorCode::out_of_memory, std::string("av_buffer_pool_get(") + what + ")");
    (*frame)->buf[0] = buf;
    (*frame)->format = fmt;
    (*frame)->width = width;
    (*frame)->height = height;
    for (int i = 0; i < 4; ++i) (*frame)->linesize[i] = pool.linesize[i];
    if (int r = av_image_fill_pointers((*frame)->data, fmt, pool.padded_height, buf->data,
                                       pool.linesize);
        r < 0) {
      return fail(ErrorCode::conversion_failed, r,
                  std::string("av_image_fill_pointers(") + what + ")");
    }
    (*frame)->extended_data = (*frame)->data;
    return std::move(*frame);
  }

  /// Tells swscale the source matrix/range so 709 content is not decoded with 601 coefficients
  /// and limited-range luma is expanded for RGB outputs; labels the output with what swscale was
  /// told to produce.
  void apply_colorspace(const AVFrame& src, AVFrame* dst, SwsContext* sws) noexcept {
    const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(src.format));
    const bool src_is_rgb = desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    const bool dst_is_rgb = is_packed_rgb(fmt_);
    const bool dst_is_gray = fmt_ == PixelFormat::gray8;

    int cs = src.colorspace;
    if (cs == AVCOL_SPC_UNSPECIFIED || cs == AVCOL_SPC_RGB) {
      cs = src.height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
    const int* coeffs = sws_getCoefficients(cs);
    const int src_full = src.color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    // RGB and gray outputs are full range. YUV outputs keep the source range; an RGB source has no
    // YUV range of its own, and the conventional (ffmpeg CLI) choice for RGB->YUV is limited.
    const int dst_full = (dst_is_rgb || dst_is_gray) ? 1 : (src_is_rgb ? 0 : src_full);
    // Always set: even for RGB sources dst_range decides whether swscale writes limited or
    // full-range YUV, and the label below must match. A negative return means "not applicable"
    // (same-format copy).
    (void)sws_setColorspaceDetails(sws, coeffs, src_full, coeffs, dst_full, 0, 1 << 16, 1 << 16);
    dst->colorspace = static_cast<AVColorSpace>(dst_is_rgb ? AVCOL_SPC_RGB : cs);
    dst->color_range = dst_full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
  }

  PixelFormat fmt_;
  int sws_flags_;
  std::optional<Size> max_;
  bool apply_sar_;
  int rotation_;
  bool mirror_;
  bool strip_display_matrix_;
  struct Scaler {
    int sw{0}, sh{0};
    AVPixelFormat sfmt{AV_PIX_FMT_NONE};
    int dw{0}, dh{0};
    std::uint64_t used{0};
    SwsCtxPtr ctx;
  };
  struct Pool {
    AVPixelFormat fmt{AV_PIX_FMT_NONE};
    int width{0}, height{0};
    int linesize[4]{};
    int padded_height{0};
    std::uint64_t used{0};
    av_ptr<AVBufferPool, av_buffer_pool_uninit> pool;
  };
  std::uint64_t clock_{0};         // least-recently-used stamps
  std::array<Scaler, 4> scalers_;  // one scaling context per output geometry seen recently
  std::array<Pool, 4> pools_;      // one buffer pool per output geometry seen recently
  SwsCtxPtr sws_replane_;
};

/// Downloads a hardware frame into a new software frame (props copied), allocating the destination.
[[nodiscard]] inline std::expected<FramePtr, Error> transfer_to_software_unpooled(
    const AVFrame& hw) {
  auto sw = make_frame();
  if (!sw) return std::unexpected(sw.error());
  if (int r = decoder_hooks().hwframe_transfer_data(sw->get(), &hw, 0); r < 0) {
    return fail(ErrorCode::conversion_failed, r, "av_hwframe_transfer_data");
  }
  if (int r = av_frame_copy_props(sw->get(), &hw); r < 0) {
    return fail(ErrorCode::conversion_failed, r, "av_frame_copy_props(transfer)");
  }
  return std::move(*sw);
}

}  // namespace stills::detail

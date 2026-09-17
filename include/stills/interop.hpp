#pragma once
// stills/interop.hpp — OPT-IN escape hatch to raw libav types. No other public header names an
// FFmpeg type. Include this one only where you genuinely need the AVFrame.

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "stills/detail/ffmpeg.hpp"
#include "stills/detail/hw.hpp"
#include "stills/image.hpp"

namespace stills::interop {

using detail::from_av;
using detail::to_av;

/// Borrow the underlying frame. Valid while the Image lives and is not moved from.
[[nodiscard]] inline const AVFrame* native_frame(const Image& img) noexcept {
  return detail::ImageAccess::frame(img);
}

/// Take ownership of the underlying frame; the Image becomes empty. Free with av_frame_free().
[[nodiscard]] inline AVFrame* release(Image&& img) noexcept {
  return detail::ImageAccess::release(img);
}

/// Hardware device families that can actually be initialised on this machine (default device).
/// Diagnostic only: creates and releases a device context per family, which is slow with a discrete
/// GPU driver, and starts driver threads for Vulkan and OpenCL.
/// Nothing on a request path calls it; call it once, off the hot path.
[[nodiscard]] inline std::vector<HardwareDeviceType> available_device_types() {
  return detail::probe_available_hw_types();
}

/// Wrap an already-decoded software frame you own. On success ownership transfers to the Image;
/// on failure the frame is left untouched (still yours).
///
/// Preconditions (rejected with invalid_argument): the format is one of PixelFormat; the frame
/// owns its pixels through reference-counted buffers (`frame->buf[0] != nullptr`, as produced by
/// av_frame_get_buffer or a decoder — a frame whose data points at memory you manage cannot be
/// freed by the Image); every plane the format needs has data and a non-negative line size.
[[nodiscard]] inline std::expected<Image, Error> adopt_frame(AVFrame* frame,
                                                             Rational time_base = {1, 1}) {
  if (frame == nullptr) return detail::fail(ErrorCode::invalid_argument, "adopt_frame: null frame");
  const auto fmt = from_av(static_cast<AVPixelFormat>(frame->format));
  if (!fmt)
    return detail::fail(ErrorCode::invalid_argument, "adopt_frame: unsupported pixel format");
  if (frame->hw_frames_ctx != nullptr) {
    return detail::fail(ErrorCode::invalid_argument,
                        "adopt_frame: hardware frames are not supported");
  }
  if (frame->data[0] == nullptr || frame->width <= 0 || frame->height <= 0) {
    return detail::fail(ErrorCode::invalid_argument, "adopt_frame: frame has no pixel data");
  }
  if (frame->buf[0] == nullptr) {
    return detail::fail(ErrorCode::invalid_argument,
                        "adopt_frame: frame does not own its pixels (buf[0] is null); allocate "
                        "with av_frame_get_buffer");
  }
  for (int p = 0; p < plane_count(*fmt); ++p) {
    if (frame->data[p] == nullptr) {
      return detail::fail(ErrorCode::invalid_argument, "adopt_frame: plane " + std::to_string(p) +
                                                           " has no data for this pixel format");
    }
    if (frame->linesize[p] < 0) {
      return detail::fail(ErrorCode::invalid_argument,
                          "adopt_frame: bottom-up frames (negative line size) are not supported");
    }
  }
  const Time t = Time::from_timestamp(frame->pts, time_base);
  const bool key = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
  return detail::ImageAccess::make(detail::FramePtr{frame}, *fmt, from_av(frame->color_range),
                                   t.is_valid() ? t : Time::zero(), key, Adjustment::none, false);
}

}  // namespace stills::interop

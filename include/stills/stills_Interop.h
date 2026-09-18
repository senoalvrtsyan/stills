#pragma once
// stills/stills_Interop.h — OPT-IN escape hatch to raw libav types. No other public header names an
// FFmpeg type. Include this one only where you genuinely need the AVFrame.

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_HardwareSupport.h"
#include "stills/stills_Image.h"

namespace stills::interop
{

using detail::fromAv;
using detail::toAv;

/// Borrow the underlying frame. Valid while the Image lives and is not moved from.
[[nodiscard]] inline const AVFrame* getNativeFrame (const Image& img) noexcept
{
    return detail::ImageAccess::getFrame (img);
}

/// Take ownership of the underlying frame; the Image becomes empty. Free with av_frame_free().
[[nodiscard]] inline AVFrame* release (Image&& img) noexcept
{
    return detail::ImageAccess::release (img);
}

/// Hardware device families that can actually be initialised on this machine (default device).
/// Diagnostic only: creates and releases a device context per family, which is slow with a discrete
/// GPU driver, and starts driver threads for Vulkan and OpenCL.
/// Nothing on a request path calls it; call it once, off the hot path.
[[nodiscard]] inline std::vector<HardwareDeviceType> getAvailableDeviceTypes()
{
    return detail::probeAvailableHwTypes();
}

/// Wrap an already-decoded software frame you own. On success ownership transfers to the Image;
/// on failure the frame is left untouched (still yours).
///
/// Preconditions (rejected with invalidArgument): the format is one of PixelFormat; the frame
/// owns its pixels through reference-counted buffers (`frame->buf[0] != nullptr`, as produced by
/// av_frame_get_buffer or a decoder — a frame whose data points at memory you manage cannot be
/// freed by the Image); every plane the format needs has data and a non-negative line size.
[[nodiscard]] inline std::expected<Image, Error> adoptFrame (AVFrame* frame, Rational timeBase = { 1, 1 })
{
    if (frame == nullptr) return detail::fail (ErrorCode::invalidArgument, "adoptFrame: null frame");
    const auto fmt = fromAv (static_cast<AVPixelFormat> (frame->format));

    if (! fmt) return detail::fail (ErrorCode::invalidArgument, "adoptFrame: unsupported pixel format");
    if (frame->hw_frames_ctx != nullptr)
    {
        return detail::fail (ErrorCode::invalidArgument, "adoptFrame: hardware frames are not supported");
    }

    if (frame->data[0] == nullptr || frame->width <= 0 || frame->height <= 0)
    {
        return detail::fail (ErrorCode::invalidArgument, "adoptFrame: frame has no pixel data");
    }

    if (frame->buf[0] == nullptr)
    {
        return detail::fail (ErrorCode::invalidArgument,
                             "adoptFrame: frame does not own its pixels (buf[0] is null); allocate "
                             "with av_frame_get_buffer");
    }

    for (int p = 0; p < getPlaneCount (*fmt); ++p)
    {
        if (frame->data[p] == nullptr)
        {
            return detail::fail (ErrorCode::invalidArgument,
                                 "adoptFrame: plane " + std::to_string (p) + " has no data for this pixel format");
        }

        if (frame->linesize[p] < 0)
        {
            return detail::fail (ErrorCode::invalidArgument,
                                 "adoptFrame: bottom-up frames (negative line size) are not supported");
        }
    }

    const Time t = Time::fromTimestamp (frame->pts, timeBase);
    const bool key = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
    return detail::ImageAccess::make (detail::FramePtr{ frame }, *fmt, fromAv (frame->color_range),
                                      t.isValid() ? t : Time::zero(), key, Adjustment::none, false);
}

} // namespace stills::interop

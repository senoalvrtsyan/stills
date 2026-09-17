#pragma once
// stills/stills_Image.h — the decoded, converted still frame handed to the caller.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "stills/detail/stills_FFmpeg.h"
#include "stills/stills_Error.h"
#include "stills/stills_Geometry.h"
#include "stills/stills_PixelFormat.h"
#include "stills/stills_Time.h"

namespace stills
{

namespace detail
{
struct ImageAccess;
}

/// Why the frame returned is not the frame asked for. `wasClamped()` is `getAdjustment() != none`;
/// this says which of the three unrelated conditions it was, which the single bool could not.
enum class Adjustment : std::uint8_t
{
    none,               ///< the frame asked for
    clampedToLast,      ///< the request was past the last frame (OutOfRangePolicy::clampToLastFrame)
    clampedToFirst,     ///< the request was before the first presented frame
    keyframeBeforeEdit, ///< nearest-keyframe mode: every keyframe at or before the request was
                        ///< trimmed away by an edit list, so the first presented frame is the
                        ///< closest answer. The frame *is* correct for the request in this case.
};

[[nodiscard]] constexpr std::string_view toString (Adjustment a) noexcept
{
    switch (a)
    {
    case Adjustment::none:
        return "none";
    case Adjustment::clampedToLast:
        return "clampedToLast";
    case Adjustment::clampedToFirst:
        return "clampedToFirst";
    case Adjustment::keyframeBeforeEdit:
        return "keyframeBeforeEdit";
    }

    return "unknown";
}

/// An owning, move-only still image. Wraps the converted AVFrame zero-copy: pixel memory lives
/// until the Image is destroyed, is never shared with the decoder, and may be read from any
/// thread. Rows may be padded; use getRowStride() or the packed-copy helpers.
class Image
{
public:
    Image (Image&&) noexcept = default;
    Image& operator= (Image&&) noexcept = default;
    Image (const Image&) = delete;
    Image& operator= (const Image&) = delete;
    ~Image() = default;

    /// True after being moved from.
    [[nodiscard]] bool isEmpty() const noexcept { return frame == nullptr; }
    [[nodiscard]] Size getSize() const noexcept { return isEmpty() ? Size{} : Size{ frame->width, frame->height }; }
    [[nodiscard]] int getWidth() const noexcept { return getSize().width; }
    [[nodiscard]] int getHeight() const noexcept { return getSize().height; }
    [[nodiscard]] PixelFormat getPixelFormat() const noexcept { return format; }
    [[nodiscard]] ColorRange getColorRange() const noexcept { return range; }
    [[nodiscard]] int getPlaneCount() const noexcept { return isEmpty() ? 0 : stills::getPlaneCount (format); }

    /// Dimensions of a plane in sample cells (chroma planes of 4:2:0 formats are half size).
    [[nodiscard]] Size getPlaneSize (int plane) const noexcept
    {
        if (isEmpty() || plane < 0 || plane >= getPlaneCount()) return {};
        const int shift = plane == 0 ? 0 : getChromaShift (format);
        return Size{ (frame->width + (1 << shift) - 1) >> shift, (frame->height + (1 << shift) - 1) >> shift };
    }

    /// Bytes between the starts of consecutive rows of `plane`.
    [[nodiscard]] std::size_t getRowStride (int plane) const noexcept
    {
        if (isEmpty() || plane < 0 || plane >= getPlaneCount()) return 0;
        const int ls = frame->linesize[plane];
        return static_cast<std::size_t> (ls < 0 ? -ls : ls);
    }

    /// The bytes of `plane`: getRowStride(plane) * getPlaneSize(plane).height bytes.
    [[nodiscard]] std::span<const std::byte> getPlane (int plane) const noexcept
    {
        if (isEmpty() || plane < 0 || plane >= getPlaneCount()) return {};
        const auto rows = static_cast<std::size_t> (getPlaneSize (plane).height);
        return { reinterpret_cast<const std::byte*> (frame->data[plane]), getRowStride (plane) * rows };
    }

    /// Convenience for single-plane formats: getPlane(0).
    [[nodiscard]] std::span<const std::byte> getPixels() const noexcept { return getPlane (0); }

    /// True when every plane's stride equals its minimal row size.
    [[nodiscard]] bool isTightlyPacked() const noexcept
    {
        for (int p = 0; p < getPlaneCount(); ++p)
        {
            const auto minimal = static_cast<std::size_t> (getPlaneSize (p).width)
                                 * static_cast<std::size_t> (getBytesPerPixel (format, p));

            if (getRowStride (p) != minimal) return false;
        }

        return ! isEmpty();
    }

    /// Size of the tightly packed representation (all planes, no padding).
    [[nodiscard]] std::size_t getPackedSizeBytes() const noexcept
    {
        if (isEmpty()) return 0;
        const int n = av_image_get_buffer_size (detail::toAv (format), frame->width, frame->height, 1);
        return n < 0 ? 0 : static_cast<std::size_t> (n);
    }

    /// Copies all planes, tightly packed and in plane order, into `dst`. (Not noexcept: the error
    /// message allocates.)
    [[nodiscard]] std::expected<void, Error> copyPackedTo (std::span<std::byte> dst) const
    {
        if (isEmpty()) return detail::fail (ErrorCode::invalidState, "Image is empty");
        const std::size_t need = getPackedSizeBytes();

        if (dst.size() < need)
        {
            return detail::fail (ErrorCode::invalidArgument,
                                 "destination too small: need " + std::to_string (need) + " bytes");
        }

        const int r =
            av_image_copy_to_buffer (reinterpret_cast<std::uint8_t*> (dst.data()), static_cast<int> (need), frame->data,
                                     frame->linesize, detail::toAv (format), frame->width, frame->height, 1);

        if (r < 0) return detail::fail (ErrorCode::conversionFailed, r, "av_image_copy_to_buffer");
        return {};
    }

    /// Allocating convenience over copyPackedTo().
    [[nodiscard]] std::expected<std::vector<std::byte>, Error> toPackedBytes() const
    {
        std::vector<std::byte> out (getPackedSizeBytes());

        if (auto r = copyPackedTo (out); ! r) return std::unexpected (std::move (r.error()));
        return out;
    }

    /// One row of `plane` (getRowStride bytes, including padding); empty when out of range.
    [[nodiscard]] std::span<const std::byte> getRow (int plane, int y) const noexcept
    {
        if (y < 0 || y >= getPlaneSize (plane).height) return {};
        const auto all = this->getPlane (plane);
        const std::size_t stride = getRowStride (plane);
        return all.subspan (static_cast<std::size_t> (y) * stride, stride);
    }

    /// Sample aspect ratio of the pixels: 1:1 after Options::applySampleAspectRatio (the default),
    /// otherwise the source's (transposed when rotated by a quarter turn).
    [[nodiscard]] Rational getSampleAspectRatio() const noexcept
    {
        if (isEmpty() || frame->sample_aspect_ratio.num <= 0 || frame->sample_aspect_ratio.den <= 0) return { 1, 1 };
        return Rational{ frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den };
    }

    /// Deep copy.
    [[nodiscard]] std::expected<Image, Error> clone() const
    {
        if (isEmpty()) return detail::fail (ErrorCode::invalidState, "Image is empty");
        auto f = detail::makeFrame();

        if (! f) return std::unexpected (f.error());
        (*f)->format = frame->format;
        (*f)->width = frame->width;
        (*f)->height = frame->height;

        if (int r = av_frame_get_buffer (f->get(), 0); r < 0)
        {
            return detail::fail (ErrorCode::outOfMemory, r, "av_frame_get_buffer");
        }

        if (int r = av_frame_copy (f->get(), frame.get()); r < 0)
        {
            return detail::fail (ErrorCode::conversionFailed, r, "av_frame_copy");
        }

        if (int r = av_frame_copy_props (f->get(), frame.get()); r < 0)
        {
            return detail::fail (ErrorCode::conversionFailed, r, "av_frame_copy_props");
        }

        Image copy{ std::move (*f), format, range, actualTime, keyframe, adjustment, corrupt };
        copy.duration = duration;
        return copy;
    }

    /// Presentation time of the frame actually returned, exact in the stream's time base
    /// (Apple's `actualTime`). Zero is the first frame of the asset.
    [[nodiscard]] Time getActualTime() const noexcept { return actualTime; }
    /// Display duration of the frame (the container's per-frame duration, else the average frame
    /// interval); zero when unknown. getActualTime() + getDuration() is the next frame's time.
    [[nodiscard]] Time getDuration() const noexcept { return duration; }
    /// True when the returned frame is a keyframe.
    [[nodiscard]] bool isKeyframe() const noexcept { return keyframe; }
    /// True when the returned frame is not the frame asked for. Which of the three conditions it was
    /// is getAdjustment(); this stays as the one-bit question.
    [[nodiscard]] bool wasClamped() const noexcept { return adjustment != Adjustment::none; }
    /// Why the frame differs from the request (Adjustment::none when it does not).
    [[nodiscard]] Adjustment getAdjustment() const noexcept { return adjustment; }
    /// True when the decoder flagged the frame as concealed/corrupt (returned only as a last resort).
    [[nodiscard]] bool isCorrupt() const noexcept { return corrupt; }

private:
    friend struct detail::ImageAccess;

    Image (detail::FramePtr frame, PixelFormat format, ColorRange range, Time actualTime, bool keyframe,
           Adjustment adjustment, bool corrupt) noexcept
      : frame (std::move (frame)), format (format), range (range), actualTime (actualTime), keyframe (keyframe),
        adjustment (adjustment), corrupt (corrupt)
    {
    }

    detail::FramePtr frame;
    PixelFormat format{ PixelFormat::rgba };
    ColorRange range{ ColorRange::unspecified };
    Time actualTime;
    bool keyframe{ false };
    Adjustment adjustment{ Adjustment::none };
    bool corrupt{ false };
    Time duration{};
};

namespace detail
{
/// Internal constructor/accessor gateway (also used by <stills/stills_Interop.h>).
struct ImageAccess
{
    [[nodiscard]] static Image make (FramePtr frame, PixelFormat format, ColorRange range, Time actualTime,
                                     bool keyframe, Adjustment adjustment, bool corrupt) noexcept
    {
        return Image{ std::move (frame), format, range, actualTime, keyframe, adjustment, corrupt };
    }

    [[nodiscard]] static const AVFrame* getFrame (const Image& img) noexcept { return img.frame.get(); }
    [[nodiscard]] static AVFrame* release (Image& img) noexcept { return img.frame.release(); }
    static void setDuration (Image& img, Time d) noexcept { img.duration = d; }
};
} // namespace detail

} // namespace stills

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
STILLS_DEFINE_ENUM_FORMATTER (stills::Adjustment);
#endif

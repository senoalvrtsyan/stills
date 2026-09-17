#pragma once
// stills/image.hpp — the decoded, converted still frame handed to the caller.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "stills/detail/ffmpeg.hpp"
#include "stills/error.hpp"
#include "stills/geometry.hpp"
#include "stills/pixel_format.hpp"
#include "stills/time.hpp"

namespace stills {

namespace detail {
struct ImageAccess;
}

/// Why the frame returned is not the frame asked for. `was_clamped()` is `adjustment() != none`;
/// this says which of the three unrelated conditions it was, which the single bool could not.
enum class Adjustment : std::uint8_t {
  none,             ///< the frame asked for
  clamped_to_last,  ///< the request was past the last frame (OutOfRangePolicy::clamp_to_last_frame)
  clamped_to_first,      ///< the request was before the first presented frame
  keyframe_before_edit,  ///< nearest-keyframe mode: every keyframe at or before the request was
                         ///< trimmed away by an edit list, so the first presented frame is the
                         ///< closest answer. The frame *is* correct for the request in this case.
};

[[nodiscard]] constexpr std::string_view to_string(Adjustment a) noexcept {
  switch (a) {
    case Adjustment::none:
      return "none";
    case Adjustment::clamped_to_last:
      return "clamped_to_last";
    case Adjustment::clamped_to_first:
      return "clamped_to_first";
    case Adjustment::keyframe_before_edit:
      return "keyframe_before_edit";
  }
  return "unknown";
}

/// An owning, move-only still image. Wraps the converted AVFrame zero-copy: pixel memory lives
/// until the Image is destroyed, is never shared with the decoder, and may be read from any
/// thread. Rows may be padded; use row_stride() or the packed-copy helpers.
class Image {
 public:
  Image(Image&&) noexcept = default;
  Image& operator=(Image&&) noexcept = default;
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  ~Image() = default;

  /// True after being moved from.
  [[nodiscard]] bool empty() const noexcept { return frame_ == nullptr; }
  [[nodiscard]] Size size() const noexcept {
    return empty() ? Size{} : Size{frame_->width, frame_->height};
  }
  [[nodiscard]] int width() const noexcept { return size().width; }
  [[nodiscard]] int height() const noexcept { return size().height; }
  [[nodiscard]] PixelFormat pixel_format() const noexcept { return format_; }
  [[nodiscard]] ColorRange color_range() const noexcept { return range_; }
  [[nodiscard]] int plane_count() const noexcept {
    return empty() ? 0 : stills::plane_count(format_);
  }

  /// Dimensions of a plane in sample cells (chroma planes of 4:2:0 formats are half size).
  [[nodiscard]] Size plane_size(int plane) const noexcept {
    if (empty() || plane < 0 || plane >= plane_count()) return {};
    const int shift = plane == 0 ? 0 : chroma_shift(format_);
    return Size{(frame_->width + (1 << shift) - 1) >> shift,
                (frame_->height + (1 << shift) - 1) >> shift};
  }

  /// Bytes between the starts of consecutive rows of `plane`.
  [[nodiscard]] std::size_t row_stride(int plane) const noexcept {
    if (empty() || plane < 0 || plane >= plane_count()) return 0;
    const int ls = frame_->linesize[plane];
    return static_cast<std::size_t>(ls < 0 ? -ls : ls);
  }

  /// The bytes of `plane`: row_stride(plane) * plane_size(plane).height bytes.
  [[nodiscard]] std::span<const std::byte> plane(int plane) const noexcept {
    if (empty() || plane < 0 || plane >= plane_count()) return {};
    const auto rows = static_cast<std::size_t>(plane_size(plane).height);
    return {reinterpret_cast<const std::byte*>(frame_->data[plane]), row_stride(plane) * rows};
  }

  /// Convenience for single-plane formats: plane(0).
  [[nodiscard]] std::span<const std::byte> pixels() const noexcept { return plane(0); }

  /// True when every plane's stride equals its minimal row size.
  [[nodiscard]] bool is_tightly_packed() const noexcept {
    for (int p = 0; p < plane_count(); ++p) {
      const auto minimal = static_cast<std::size_t>(plane_size(p).width) *
                           static_cast<std::size_t>(bytes_per_pixel(format_, p));
      if (row_stride(p) != minimal) return false;
    }
    return !empty();
  }

  /// Size of the tightly packed representation (all planes, no padding).
  [[nodiscard]] std::size_t packed_size_bytes() const noexcept {
    if (empty()) return 0;
    const int n =
        av_image_get_buffer_size(detail::to_av(format_), frame_->width, frame_->height, 1);
    return n < 0 ? 0 : static_cast<std::size_t>(n);
  }

  /// Copies all planes, tightly packed and in plane order, into `dst`. (Not noexcept: the error
  /// message allocates.)
  [[nodiscard]] std::expected<void, Error> copy_packed_to(std::span<std::byte> dst) const {
    if (empty()) return detail::fail(ErrorCode::invalid_state, "Image is empty");
    const std::size_t need = packed_size_bytes();
    if (dst.size() < need) {
      return detail::fail(ErrorCode::invalid_argument,
                          "destination too small: need " + std::to_string(need) + " bytes");
    }
    const int r = av_image_copy_to_buffer(reinterpret_cast<std::uint8_t*>(dst.data()),
                                          static_cast<int>(need), frame_->data, frame_->linesize,
                                          detail::to_av(format_), frame_->width, frame_->height, 1);
    if (r < 0) return detail::fail(ErrorCode::conversion_failed, r, "av_image_copy_to_buffer");
    return {};
  }

  /// Allocating convenience over copy_packed_to().
  [[nodiscard]] std::expected<std::vector<std::byte>, Error> to_packed_bytes() const {
    std::vector<std::byte> out(packed_size_bytes());
    if (auto r = copy_packed_to(out); !r) return std::unexpected(std::move(r.error()));
    return out;
  }

  /// One row of `plane` (row_stride bytes, including padding); empty when out of range.
  [[nodiscard]] std::span<const std::byte> row(int plane, int y) const noexcept {
    if (y < 0 || y >= plane_size(plane).height) return {};
    const auto all = this->plane(plane);
    const std::size_t stride = row_stride(plane);
    return all.subspan(static_cast<std::size_t>(y) * stride, stride);
  }

  /// Sample aspect ratio of the pixels: 1:1 after Options::apply_sample_aspect_ratio (the default),
  /// otherwise the source's (transposed when rotated by a quarter turn).
  [[nodiscard]] Rational sample_aspect_ratio() const noexcept {
    if (empty() || frame_->sample_aspect_ratio.num <= 0 || frame_->sample_aspect_ratio.den <= 0)
      return {1, 1};
    return Rational{frame_->sample_aspect_ratio.num, frame_->sample_aspect_ratio.den};
  }

  /// Deep copy.
  [[nodiscard]] std::expected<Image, Error> clone() const {
    if (empty()) return detail::fail(ErrorCode::invalid_state, "Image is empty");
    auto f = detail::make_frame();
    if (!f) return std::unexpected(f.error());
    (*f)->format = frame_->format;
    (*f)->width = frame_->width;
    (*f)->height = frame_->height;
    if (int r = av_frame_get_buffer(f->get(), 0); r < 0) {
      return detail::fail(ErrorCode::out_of_memory, r, "av_frame_get_buffer");
    }
    if (int r = av_frame_copy(f->get(), frame_.get()); r < 0) {
      return detail::fail(ErrorCode::conversion_failed, r, "av_frame_copy");
    }
    if (int r = av_frame_copy_props(f->get(), frame_.get()); r < 0) {
      return detail::fail(ErrorCode::conversion_failed, r, "av_frame_copy_props");
    }
    Image copy{std::move(*f), format_, range_, actual_time_, keyframe_, adjustment_, corrupt_};
    copy.duration_ = duration_;
    return copy;
  }

  /// Presentation time of the frame actually returned, exact in the stream's time base
  /// (Apple's `actualTime`). Zero is the first frame of the asset.
  [[nodiscard]] Time actual_time() const noexcept { return actual_time_; }
  /// Display duration of the frame (the container's per-frame duration, else the average frame
  /// interval); zero when unknown. actual_time() + duration() is the next frame's time.
  [[nodiscard]] Time duration() const noexcept { return duration_; }
  /// True when the returned frame is a keyframe.
  [[nodiscard]] bool is_keyframe() const noexcept { return keyframe_; }
  /// True when the returned frame is not the frame asked for. Which of the three conditions it was
  /// is adjustment(); this stays as the one-bit question.
  [[nodiscard]] bool was_clamped() const noexcept { return adjustment_ != Adjustment::none; }
  /// Why the frame differs from the request (Adjustment::none when it does not).
  [[nodiscard]] Adjustment adjustment() const noexcept { return adjustment_; }
  /// True when the decoder flagged the frame as concealed/corrupt (returned only as a last resort).
  [[nodiscard]] bool is_corrupt() const noexcept { return corrupt_; }

 private:
  friend struct detail::ImageAccess;

  Image(detail::FramePtr frame, PixelFormat format, ColorRange range, Time actual_time,
        bool keyframe, Adjustment adjustment, bool corrupt) noexcept
      : frame_(std::move(frame)),
        format_(format),
        range_(range),
        actual_time_(actual_time),
        keyframe_(keyframe),
        adjustment_(adjustment),
        corrupt_(corrupt) {}

  detail::FramePtr frame_;
  PixelFormat format_{PixelFormat::rgba};
  ColorRange range_{ColorRange::unspecified};
  Time actual_time_;
  bool keyframe_{false};
  Adjustment adjustment_{Adjustment::none};
  bool corrupt_{false};
  Time duration_{};
};

namespace detail {
/// Internal constructor/accessor gateway (also used by <stills/interop.hpp>).
struct ImageAccess {
  [[nodiscard]] static Image make(FramePtr frame, PixelFormat format, ColorRange range,
                                  Time actual_time, bool keyframe, Adjustment adjustment,
                                  bool corrupt) noexcept {
    return Image{std::move(frame), format, range, actual_time, keyframe, adjustment, corrupt};
  }
  [[nodiscard]] static const AVFrame* frame(const Image& img) noexcept { return img.frame_.get(); }
  [[nodiscard]] static AVFrame* release(Image& img) noexcept { return img.frame_.release(); }
  static void set_duration(Image& img, Time d) noexcept { img.duration_ = d; }
};
}  // namespace detail

}  // namespace stills

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
STILLS_DEFINE_ENUM_FORMATTER(stills::Adjustment);
#endif

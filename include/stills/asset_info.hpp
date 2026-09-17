#pragma once
// stills/asset_info.hpp — read-only facts about an opened asset. No FFmpeg dependency.

#include <cstdint>
#include <optional>
#include <string>

#include "stills/geometry.hpp"
#include "stills/options.hpp"
#include "stills/time.hpp"

namespace stills {

/// Immutable description of the opened asset, captured at open().
struct AssetInfo {
  std::string container_name = {};       ///< AVInputFormat::name, e.g. "mov,mp4,m4a,3gp,3g2,mj2"
  std::string codec_name = {};           ///< e.g. "h264"
  std::string source_pixel_format = {};  ///< e.g. "yuv420p"
  int video_stream_index{-1};
  Rational time_base = {};           ///< the stream's time base (actual_time() is exact in it)
  Rational average_frame_rate = {};  ///< {0,1} when unknown
  std::optional<Time> duration =
      std::nullopt;  ///< stream duration, else container duration, else nullopt
  std::optional<std::int64_t> frame_count =
      std::nullopt;        ///< container-declared frame count when known
  Size coded_size = {};    ///< as stored in the stream
  Size display_size = {};  ///< after sample-aspect-ratio and rotation
  Size output_size = {};   ///< display_size fitted into Options::maximum_size
  /// Sample aspect ratio in effect (container declaration, else bitstream), 1:1 when square.
  Rational sample_aspect_ratio = {1, 1};
  int rotation_degrees{0};  ///< 0/90/180/270 *clockwise* rotation applied for upright display (a
                            ///< 90° CCW display matrix reports 270)
  /// The display matrix also mirrors the picture horizontally (after the rotation). Applied
  /// together with the rotation when Options::apply_preferred_track_transform is set.
  bool mirrored{false};
  /// The container can be positioned by timestamp. False for pipes and raw elementary streams,
  /// whose backward requests re-open the input or fail (see README "Known limitations").
  bool seekable{true};
  /// The container carries no timestamps; frames are stamped from the codec frame rate in display
  /// order (raw H.264/HEVC elementary streams). actual_time() is then a reconstruction.
  bool timestamps_synthesized{false};
  /// Colour metadata of the source as declared (libav names: "bt709", "smpte2084" (PQ),
  /// "arib-std-b67" (HLG), "bt2020", ...; empty when unspecified). The library maps the matrix and
  /// range only; a compositor must tone-map PQ/HLG itself (README "Colour").
  std::string color_transfer = {};
  std::string color_primaries = {};
  std::string color_space = {};

  /// [0, duration) when the duration is known.
  [[nodiscard]] std::optional<TimeRange> time_range() const noexcept {
    if (!duration) return std::nullopt;
    return TimeRange{Time::zero(), *duration};
  }
};

/// Which decode path ended up active after open().
struct ActiveDecoder {
  std::string decoder_name = {};  ///< AVCodec::name
  bool hardware{false};
  /// The device family, when it is one this library names; nullopt for a family FFmpeg knows but
  /// this enum does not (see device_type_name).
  std::optional<HardwareDeviceType> device_type = std::nullopt;
  std::string device_type_name = {};  ///< av_hwdevice_get_type_name, set when hardware == true
  std::string device = {};            ///< device string actually used
  std::string fallback_reason = {};  ///< why hardware was not used (prefer_hardware/automatic only)
  int decoder_threads{0};            ///< frame threads the decoder was opened with (1 for hardware)
};

}  // namespace stills

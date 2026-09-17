#pragma once
// stills/detail/pipeline.hpp — everything that touches one AVFormatContext/AVCodecContext:
// opening, stream selection, hardware setup with software fallback, accurate seeking and the
// decode loop, frame selection, and conversion to the output Image.
//
// A Pipeline is single-threaded by contract: callers serialise access (see worker.hpp).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stills/asset_info.hpp"
#include "stills/detail/convert.hpp"
#include "stills/detail/ffmpeg.hpp"
#include "stills/detail/hw.hpp"
#include "stills/image.hpp"
#include "stills/options.hpp"
#include "stills/time.hpp"

namespace stills::detail {

/// Cooperative cancellation: two optional flags (per-batch and per-generator).
struct CancelToken {
  const std::atomic<bool>* batch{nullptr};
  const std::atomic<bool>* generator{nullptr};

  [[nodiscard]] bool requested() const noexcept {
    return (batch != nullptr && batch->load(std::memory_order_relaxed)) ||
           (generator != nullptr && generator->load(std::memory_order_relaxed));
  }
};

/// Rotation and mirroring that display the coded picture upright, decoded from a display matrix.
struct DisplayTransform {
  int rotation{0};       ///< clockwise degrees, one of 0/90/180/270
  bool mirrored{false};  ///< horizontal mirror, applied *after* the rotation
};

/// Decodes a 3x3 display matrix (16.16 fixed point, row-vector convention: display = coded * M).
/// A negative determinant means the matrix contains a reflection; av_display_rotation_get alone
/// would misreport a pure horizontal flip as a 180 degree rotation. Splitting M = R * F (rotate,
/// then mirror horizontally) covers all eight orientations exactly.
[[nodiscard]] inline DisplayTransform decode_display_matrix(const std::int32_t in[9]) noexcept {
  std::int32_t m[9];
  std::memcpy(m, in, sizeof m);
  DisplayTransform t;
  const double det = static_cast<double>(m[0]) * static_cast<double>(m[4]) -
                     static_cast<double>(m[1]) * static_cast<double>(m[3]);
  if (det < 0) {
    t.mirrored = true;
    av_display_matrix_flip(m, 1, 0);  // M * Fh: negates column 0, leaving a pure rotation
  }
  // libav reports counter-clockwise degrees; ffmpeg's own autorotate negates it.
  double theta = -av_display_rotation_get(m);
  if (std::isnan(theta)) return DisplayTransform{};
  theta -= 360.0 * std::floor(theta / 360.0 + 0.9 / 360.0);
  // Only right angles are honoured; anything else (a 45 degree matrix) snaps to the nearest.
  const int snapped = static_cast<int>(std::lround(theta / 90.0)) * 90;
  t.rotation = snapped % 360;
  return t;
}

class Pipeline {
 public:
  /// A frame chosen for a request, owned by the pipeline (held_ or a pending slot).
  struct Selected {
    AVFrame* frame{nullptr};
    Adjustment adjustment{Adjustment::none};
    bool corrupt{false};
  };

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;
  Pipeline(Pipeline&&) = delete;
  Pipeline& operator=(Pipeline&&) = delete;
  /// Opens the source. `token` (optional) makes the open cancellable: libavformat's interrupt
  /// callback consults it during probing and the first decode, so a stalled network source fails
  /// with `cancelled` / `open_failed` instead of blocking.
  [[nodiscard]] static std::expected<std::unique_ptr<Pipeline>, Error> open(
      std::string source, Options options, const CancelToken* token = nullptr) {
    std::unique_ptr<Pipeline> p{new Pipeline(std::move(source), std::move(options))};
    p->current_token_ = token;
    auto finish = [&](Error e) -> std::expected<std::unique_ptr<Pipeline>, Error> {
      p->current_token_ = nullptr;
      if (token != nullptr && token->requested())
        return fail(ErrorCode::cancelled, "open cancelled");
      return std::unexpected(std::move(e));
    };
    if (auto r = p->open_input(); !r) return finish(std::move(r.error()));
    if (auto r = p->setup_decoder(); !r) return finish(std::move(r.error()));
    p->fill_info();
    p->current_token_ = nullptr;
    return p;
  }

  [[nodiscard]] const AssetInfo& info() const noexcept { return info_; }

  /// Snapshot of the active decode path. Returned by value: a lazy hardware failure may switch
  /// the path to software after open(), and readers must never observe a torn value.
  [[nodiscard]] ActiveDecoder active_decoder() const {
    std::lock_guard lk(active_mutex_);
    return active_;
  }
  [[nodiscard]] const Options& options() const noexcept { return opt_; }

  /// Extracts the frame for `requested` (asset-relative). Thread-unsafe by design.
  [[nodiscard]] std::expected<Image, Error> image_at(Time requested, const CancelToken& token) {
    return image_at(requested, RequestOptions{}, token);
  }
  /// The decode path allocates freely and the async worker's loop is noexcept, so a std::bad_alloc
  /// below here would terminate instead of surfacing the out_of_memory the API defines. Caught at
  /// this frame, the only one that can drop the half-updated state: the next request re-positions.
  [[nodiscard]] std::expected<Image, Error> image_at(Time requested, const RequestOptions& ro,
                                                     const CancelToken& token) {
    cur_ = RequestStats{};
    try {
      return image_at_impl(requested, ro, token);
    } catch (const std::bad_alloc&) {
      current_token_ = nullptr;
      reset_position();     // frees the frames and the buffers; allocates nothing
      positioned_ = false;  // an unwound request left the demuxer wherever it stopped
      hw_fault_ = false;
      // Short enough to live in the string's own storage, so building it cannot allocate either.
      return std::unexpected(Error{ErrorCode::out_of_memory, 0, "out of memory"});
    }
  }

 private:
  [[nodiscard]] std::expected<Image, Error> image_at_impl(Time requested, const RequestOptions& ro,
                                                          const CancelToken& token) {
    current_token_ = &token;
    // Only a fault raised by *this* request may drive the rebuild below. One left over from a
    // candidate probed at open would otherwise turn the next unrelated failure (a cancellation, a
    // time_out_of_range) into a decoder rebuild and rewrite fallback_reason.
    hw_fault_ = false;
    const auto attempt = [&]() -> std::expected<Image, Error> {
      auto sel = select_for(requested, ro, token);
      if (!sel) return std::unexpected(std::move(sel.error()));
      return convert(*sel);
    };
    auto result = attempt();
    if (!result && hw_fault_ && hw_active_ && !hw_retried_) {
      // Hardware faults after open (an EIO on a surface download, a lost session) are usually
      // transient and clear on a fresh session, so rebuild the same hardware decoder once before
      // falling back to software.
      hw_fault_ = false;
      hw_retried_ = true;
      const std::string why = result.error().message;
      if (auto r = rebuild_hardware(); r) {
        result = attempt();
      } else {
        hw_fault_ = true;  // fall through to the software fallback below
        result = std::unexpected(make_error(
            ErrorCode::decode_failed, 0, why + "; hardware rebuild failed: " + r.error().message));
      }
    }
    if (!result && hw_fault_) {
      hw_fault_ = false;
      if (opt_.hardware.policy == HardwarePolicy::require_hardware) {
        current_token_ = nullptr;
        return std::unexpected(make_error(ErrorCode::hardware_unavailable, 0,
                                          "hardware decoding failed: " + result.error().message));
      }
      if (auto r = rebuild_software("hardware decoder failed during decoding: " +
                                    result.error().message);
          !r) {
        current_token_ = nullptr;
        return std::unexpected(std::move(r.error()));
      }
      result = attempt();
    }
    if (result && hw_active_)
      hw_retried_ = false;  // a successful hardware request re-arms the retry
    current_token_ = nullptr;
    // AVERROR_EXIT with a cancelled token is the cancellation; with a live token it is a stale
    // interrupt and stays an I/O failure.
    if (!result && result.error().av_error == k::exit_requested && token.requested()) {
      return fail(ErrorCode::cancelled, "cancelled");
    }
    return result;
  }

  /// Positions and selects the frame for `requested` without converting it (the frame stays owned
  /// by the pipeline: held_ or the pending slot). Validates the request, maps the time
  /// into stream ticks, applies the bounds policy and the tolerance window.
  [[nodiscard]] std::expected<Selected, Error> select_for(Time requested, const RequestOptions& ro,
                                                          const CancelToken& token) {
    if (auto v = ro.validate(opt_.pixel_format); !v) return std::unexpected(std::move(v.error()));
    req_max_ = ro.maximum_size;
    const Tolerance tolerance = ro.tolerance.value_or(opt_.tolerance);
    if (!requested.is_finite()) {
      return fail(ErrorCode::invalid_argument,
                  "requested time is not finite: " + to_string(requested));
    }
    if (requested.is_negative()) {
      return fail(ErrorCode::invalid_argument,
                  "requested time is negative: " + to_string(requested));
    }
    // Nearest tick, not floor: coarse time bases (Matroska/WebM: 1 ms) store k/fps rounded to the
    // nearest tick, so an exact k/fps lands a fraction of a tick below frame k and flooring would
    // return frame k-1.
    const std::int64_t rel =
        requested.to_timestamp(from_av(stream_.time_base), TimeRounding::nearest);
    if (rel == std::numeric_limits<std::int64_t>::max()) {
      return fail(ErrorCode::time_out_of_range, "requested time does not fit the stream time base");
    }
    std::int64_t target = 0;
    if (__builtin_add_overflow(stream_.start_pts, rel, &target)) {
      // A representable Time can still fall outside the stream's timestamp domain once the origin
      // is added (an MPEG-TS starting at 10 s). Report it rather than wrapping.
      return fail(ErrorCode::time_out_of_range, "requested time does not fit the stream time base");
    }
    Adjustment clamped = Adjustment::none;
    // Containers occasionally understate their duration by a frame; give one frame of slack before
    // rejecting up front, and let the decoder (EOF) decide inside that margin.
    if (stream_.duration_pts &&
        rel > *stream_.duration_pts + std::max<std::int64_t>(stream_.frame_duration_hint, 0)) {
      if (opt_.out_of_range == OutOfRangePolicy::error) {
        return fail(
            ErrorCode::time_out_of_range,
            to_string(requested) + " is beyond the asset duration " +
                to_string(Time::from_timestamp(*stream_.duration_pts, from_av(stream_.time_base))));
      }
      target = stream_.start_pts + *stream_.duration_pts;
      clamped = Adjustment::clamped_to_last;
    }

    // Tolerance window in stream ticks (validate() rejects negative tolerances).
    const auto window_edge = [&](Time tol, bool is_before) -> std::int64_t {
      if (tol.is_positive_infinity()) {
        return is_before ? std::numeric_limits<std::int64_t>::min()
                         : std::numeric_limits<std::int64_t>::max();
      }
      std::int64_t ticks = 0;
      if (tol.is_finite() && tol > Time::zero()) {
        ticks = tol.to_timestamp(from_av(stream_.time_base), TimeRounding::down);
      }
      // A tolerance that reaches beyond half the timestamp domain is treated as an infinite one.
      // The edge is formed first and compared afterwards: `ticks > target - INT64_MIN / 2` is the
      // same inequality, but forms `target - INT64_MIN / 2` even when the tolerance is zero, which
      // overflows for a target past INT64_MAX / 2 — reachable from image_at() on a source with no
      // declared duration, where the bounds check above cannot reject the request first.
      std::int64_t edge = 0;
      if (is_before) {
        if (__builtin_sub_overflow(target, ticks, &edge) ||
            edge < std::numeric_limits<std::int64_t>::min() / 2) {
          return std::numeric_limits<std::int64_t>::min();
        }
        return edge;
      }
      if (__builtin_add_overflow(target, ticks, &edge) ||
          edge > std::numeric_limits<std::int64_t>::max() / 2) {
        return std::numeric_limits<std::int64_t>::max();
      }
      return edge;
    };
    const std::int64_t lo = window_edge(tolerance.before, true);
    const std::int64_t hi = window_edge(tolerance.after, false);

    if (!fmt_ || !codec_) {
      // A previous re-open failed (or was cancelled): retry rather than staying dead forever.
      if (auto r = reopen(); !r) {
        return fail(ErrorCode::unusable, r.error().av_error,
                    "the generator is unusable: " + broken_reason_);
      }
    }
    if (!recover_after_interrupt()) {
      // The interrupt left libavio's state stuck and this libavformat major is not one
      // tryResetIoState() can clear (see there): re-open instead of reading through it.
      if (auto r = reopen(); !r) return std::unexpected(std::move(r.error()));
    }
    eof_retried_ = false;  // one re-position per attempt (a hardware fallback retries the request)
    return extract(target, lo, hi, clamped, token);
  }

  struct StreamInfo {
    int index{-1};
    AVRational time_base{1, 1};
    std::int64_t start_pts{0};
    std::optional<std::int64_t> duration_pts;
    std::int64_t frame_duration_hint{0};
    AVRational container_sar{0, 1};  ///< AVStream::sample_aspect_ratio (container level, wins)
    AVRational codec_sar{0, 1};      ///< AVCodecParameters::sample_aspect_ratio (bitstream level)
    AVRational avg_frame_rate{0, 1};
    DisplayTransform transform;
    bool seekable{true};     ///< avformat_seek_file is usable (timestamps + seekable I/O)
    bool io_seekable{true};  ///< the source can be rewound (a file, not a pipe)
    /// The container carries no timestamps (raw elementary streams). libavformat then synthesises
    /// decode-order counters that disagree with display order under B-frame reordering, so the
    /// pipeline stamps decoded frames itself: output order x frame duration.
    bool synthesize_timestamps{false};
    /// The container had a populated index right after avformat_find_stream_info (MP4, indexed
    /// AVI/FLV). Indices that grow lazily during seeks (MPEG-TS flags every probed packet as a
    /// keyframe) are never consulted for the forward-or-seek decision.
    bool index_trusted{false};
  };

  static constexpr int max_consecutive_errors = 32;
  /// Consecutive avformat_seek_file failures (never counting interrupted I/O) before the pipeline
  /// stops seeking and decodes forward / re-opens instead.
  static constexpr int max_seek_failures = 3;
  /// Hard cap on landing back-offs per request; the doubling step reaches the start long before.
  static constexpr int max_backoffs = 64;

  Pipeline(std::string source, Options options)
      : source_(std::move(source)), opt_(std::move(options)) {}

 public:
  ~Pipeline() = default;

 private:
  static int interrupt_callback(void* opaque) noexcept {
    auto* self = static_cast<Pipeline*>(opaque);
    if (self == nullptr) return 0;
    if (self->current_token_ != nullptr && self->current_token_->requested()) {
      // Recorded here because libavio's own state does not preserve it: see
      // recover_after_interrupt(). Same thread as the request that is being interrupted.
      self->interrupt_fired_ = true;
      return 1;
    }
    return 0;
  }

  [[nodiscard]] std::expected<void, Error> open_input() {
    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr) return fail(ErrorCode::out_of_memory, "avformat_alloc_context");
    raw->interrupt_callback.callback = &Pipeline::interrupt_callback;
    raw->interrupt_callback.opaque = this;
    raw->flags |= AVFMT_FLAG_GENPTS;
    DictPtr dict;
    {
      AVDictionary* d = nullptr;
      for (const auto& [key, value] : opt_.demuxer_options)
        av_dict_set(&d, key.c_str(), value.c_str(), 0);
      dict.reset(d);
    }
    AVDictionary* dict_raw = dict.release();
    // avformat_open_input frees and nulls `raw` on failure, so wrap only on success. Wrap it
    // *before* the unknown-option check: that check returns on a source that opened fine, and an
    // open context reached only through `raw` would be leaked.
    const int r = avformat_open_input(&raw, source_.c_str(), nullptr, &dict_raw);
    dict.reset(dict_raw);  // whatever was not consumed
    if (r >= 0) fmt_.reset(raw);
    if (auto bad = unknown_demuxer_options(dict.get()); !bad.empty()) {
      return fail(ErrorCode::invalid_argument,
                  "demuxer_options: no such option in this FFmpeg build: " + bad +
                      " (a key libavformat knows but does not apply to this source is accepted)");
    }
    if (r < 0) {
      ErrorCode code = ErrorCode::open_failed;
      if (r == k::enoent) code = ErrorCode::file_not_found;
      if (r == k::invalid_data) code = ErrorCode::unsupported_format;
      return fail(code, r, "avformat_open_input(\"" + source_ + "\")");
    }

    // avformat_find_stream_info() decodes to fill in what the container did not declare, so an
    // oversized frame costs memory there too. Most containers (MP4, Matroska) declare the size in
    // their header: if every video stream already declares more than the cap, refuse before that.
    if (auto r2 = check_declared_size(); !r2) return r2;
    if (int r2 = avformat_find_stream_info(fmt_.get(), nullptr); r2 < 0) {
      return fail(ErrorCode::unsupported_format, r2, "avformat_find_stream_info");
    }

    const AVCodec* codec = nullptr;
    int idx = av_find_best_stream(fmt_.get(), AVMEDIA_TYPE_VIDEO,
                                  opt_.video_stream_index.value_or(-1), -1, &codec, 0);
    if (idx == k::decoder_not_found) {
      return fail(ErrorCode::decoder_not_found, idx,
                  "av_find_best_stream: no decoder for the video stream");
    }
    if (idx < 0) {
      if (opt_.video_stream_index) {
        return fail(ErrorCode::invalid_argument, idx,
                    "stream " + std::to_string(*opt_.video_stream_index) +
                        " is not a decodable video stream");
      }
      return fail(ErrorCode::no_video_stream, idx,
                  "av_find_best_stream: no video stream in \"" + source_ + "\"");
    }
    // Cover art is a "video" stream to libavformat; not to us, unless asked for.
    if ((fmt_->streams[idx]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0 &&
        !opt_.allow_attached_pictures && !opt_.video_stream_index) {
      int alt = -1;
      for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
        AVStream* s = fmt_->streams[i];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            (s->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0 &&
            avcodec_find_decoder(s->codecpar->codec_id) != nullptr) {
          alt = static_cast<int>(i);
          break;
        }
      }
      if (alt < 0) {
        return fail(
            ErrorCode::no_video_stream,
            "the only video stream is an attached picture (set Options::allow_attached_pictures)");
      }
      idx = alt;
      codec = avcodec_find_decoder(fmt_->streams[idx]->codecpar->codec_id);
    }
    if (codec == nullptr) {
      return fail(ErrorCode::decoder_not_found, "no decoder available for the selected stream");
    }
    codec_desc_ = codec;
    st_ = fmt_->streams[idx];
    if (opt_.max_input_pixels) {
      // Before the decoder exists: setting it up and probing the first frame is what allocates for
      // the declared frame size, and a hostile file costs a few hundred bytes to declare it.
      const std::int64_t pixels = static_cast<std::int64_t>(std::max(st_->codecpar->width, 0)) *
                                  std::max(st_->codecpar->height, 0);
      if (pixels > *opt_.max_input_pixels) {
        return fail(ErrorCode::unsupported_format,
                    "the video stream is " + std::to_string(st_->codecpar->width) + "x" +
                        std::to_string(st_->codecpar->height) + " = " + std::to_string(pixels) +
                        " pixels, over Options::max_input_pixels (" +
                        std::to_string(*opt_.max_input_pixels) + ")");
      }
    }
    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
      if (static_cast<int>(i) != idx) fmt_->streams[i]->discard = AVDISCARD_ALL;
    }

    stream_ = StreamInfo{};
    stream_.index = idx;
    stream_.time_base = st_->time_base;
    if (stream_.time_base.num <= 0 || stream_.time_base.den <= 0) {
      return fail(ErrorCode::unsupported_format, "video stream has an invalid time base");
    }
    if (st_->start_time != k::no_pts) {
      stream_.start_pts = st_->start_time;
    } else if (fmt_->start_time != k::no_pts) {
      stream_.start_pts = av_rescale_q(fmt_->start_time, k::time_base_q, stream_.time_base);
    }
    if (st_->duration != k::no_pts && st_->duration > 0) {
      stream_.duration_pts = st_->duration;
    } else if (fmt_->duration != k::no_pts && fmt_->duration > 0) {
      stream_.duration_pts = av_rescale_q(fmt_->duration, k::time_base_q, stream_.time_base);
    }
    stream_.avg_frame_rate = st_->avg_frame_rate;
    const AVRational fr = st_->avg_frame_rate.num > 0 ? st_->avg_frame_rate : st_->r_frame_rate;
    if (fr.num > 0 && fr.den > 0) {
      stream_.frame_duration_hint = av_rescale_q(1, AVRational{fr.den, fr.num}, stream_.time_base);
    }
    // Same priority as av_guess_sample_aspect_ratio (and therefore ffmpeg/ffplay): the container's
    // declaration wins over the bitstream's; a frame-level SAR sits in between (see convert()).
    stream_.container_sar = st_->sample_aspect_ratio;
    stream_.codec_sar = st_->codecpar->sample_aspect_ratio;
    stream_.transform = read_transform(*st_->codecpar);
    stream_.io_seekable = fmt_->pb != nullptr && (fmt_->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    // Raw elementary streams (AVFMT_NOTIMESTAMPS) have nothing to seek by; libavformat's generic
    // seek fails and may leave the demuxer mid-file, so such inputs are decoded forward or
    // re-opened.
    stream_.seekable = stream_.io_seekable && (fmt_->iformat->flags & AVFMT_NOTIMESTAMPS) == 0 &&
                       seek_failures_ < max_seek_failures;
    stream_.synthesize_timestamps = (fmt_->iformat->flags & AVFMT_NOTIMESTAMPS) != 0;
    stream_.index_trusted = avformat_index_get_entries_count(st_) > 0;

    auto pkt = make_packet();
    if (!pkt) return std::unexpected(pkt.error());
    pkt_ = std::move(*pkt);
    pkt_pending_ = false;
    auto lpkt = make_packet();
    if (!lpkt) return std::unexpected(lpkt.error());
    land_pkt_ = std::move(*lpkt);
    for (FramePtr* f : {&recv_, &held_, &pending_, &corrupt_last_}) {
      auto fr2 = make_frame();
      if (!fr2) return std::unexpected(fr2.error());
      *f = std::move(*fr2);
    }
    const DisplayTransform applied =
        opt_.apply_preferred_track_transform ? stream_.transform : DisplayTransform{};
    converter_.emplace(opt_.pixel_format, opt_.scaler, opt_.maximum_size,
                       opt_.apply_sample_aspect_ratio, applied.rotation, applied.mirrored,
                       /*strip_display_matrix=*/opt_.apply_preferred_track_transform);
    return {};
  }

  /// Demuxer options libavformat left unconsumed *and* does not define anywhere: a misspelling.
  /// A key it defines but did not apply here is legitimate — an HTTP option that a local path
  /// never reaches — and is not reported. Returns them comma-separated, or empty.
  [[nodiscard]] static std::string unknown_demuxer_options(AVDictionary* left) {
    std::string bad;
    const AVDictionaryEntry* e = nullptr;
    while ((e = av_dict_iterate(left, e)) != nullptr) {
      if (option_exists(e->key)) continue;
      if (!bad.empty()) bad += ", ";
      bad += '"';
      bad += e->key;
      bad += '"';
    }
    return bad;
  }

  /// Whether any of libavformat's option classes (the context, the demuxers, the protocols)
  /// defines `key`.
  [[nodiscard]] static bool option_exists(const char* key) noexcept {
    const AVClass* fc = avformat_get_class();
    if (av_opt_find(&fc, key, nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ | AV_OPT_SEARCH_CHILDREN) !=
        nullptr)
      return true;
    void* it = nullptr;
    while (const AVClass* child = av_opt_child_class_iterate(fc, &it)) {
      if (av_opt_find(&child, key, nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ | AV_OPT_SEARCH_CHILDREN) !=
          nullptr)
        return true;
    }
    return false;
  }

  /// Options::max_input_pixels against what the container declared, before anything decodes. Only
  /// refuses when *every* video stream is over the cap and has a declared size: a stream whose size
  /// is unknown here is decided by the check in open_input() once stream info has been read.
  [[nodiscard]] std::expected<void, Error> check_declared_size() const {
    if (!opt_.max_input_pixels) return {};
    int worst_w = 0, worst_h = 0;
    bool any_video = false;
    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
      const AVCodecParameters& par = *fmt_->streams[i]->codecpar;
      if (par.codec_type != AVMEDIA_TYPE_VIDEO) continue;
      any_video = true;
      const std::int64_t pixels =
          static_cast<std::int64_t>(std::max(par.width, 0)) * std::max(par.height, 0);
      if (pixels <= *opt_.max_input_pixels) return {};  // one of them might be usable
      if (pixels > static_cast<std::int64_t>(worst_w) * worst_h) {
        worst_w = par.width;
        worst_h = par.height;
      }
    }
    if (!any_video || worst_w <= 0) return {};
    return fail(ErrorCode::unsupported_format,
                "the video stream is " + std::to_string(worst_w) + "x" + std::to_string(worst_h) +
                    " = " + std::to_string(static_cast<std::int64_t>(worst_w) * worst_h) +
                    " pixels, over Options::max_input_pixels (" +
                    std::to_string(*opt_.max_input_pixels) + ")");
  }

  /// The display transform from the stream's display matrix side data (codecpar->coded_side_data,
  /// the non-deprecated path on FFmpeg 6.1 and 7.x).
  [[nodiscard]] static DisplayTransform read_transform(const AVCodecParameters& par) noexcept {
    const AVPacketSideData* sd = av_packet_side_data_get(
        par.coded_side_data, par.nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (sd == nullptr || sd->size < 9 * sizeof(std::int32_t)) return DisplayTransform{};
    std::int32_t matrix[9];
    std::memcpy(matrix, sd->data, sizeof matrix);
    return decode_display_matrix(matrix);
  }

  [[nodiscard]] std::expected<void, Error> build_codec(const HwCandidate* hw) {
    hw_fault_ = false;  // belongs to the decoder being replaced
    codec_.reset();
    hw_device_.reset();
    hw_state_.reset();
    reset_position();

    CodecCtxPtr cc{avcodec_alloc_context3(codec_desc_)};
    if (!cc) return fail(ErrorCode::out_of_memory, "avcodec_alloc_context3");
    if (int r = avcodec_parameters_to_context(cc.get(), st_->codecpar); r < 0) {
      return fail(ErrorCode::decoder_open_failed, r, "avcodec_parameters_to_context");
    }
    cc->pkt_timebase = st_->time_base;  // required for best_effort_timestamp / frame->duration
    // The same cap inside libavcodec, so a resolution change mid-stream is refused by the decoder
    // rather than allocated for.
    if (opt_.max_input_pixels) cc->max_pixels = *opt_.max_input_pixels;
    // Frame threading adds latency after every flush but still wins for this seek-heavy workload.
    // Hardware decoders get one thread: hwaccel + frame threads is trouble and buys nothing.
    cc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (hw != nullptr) {
      cc->thread_count = 1;
    } else if (opt_.decoder_threads > 0) {
      cc->thread_count = opt_.decoder_threads;
    } else {
      cc->thread_count =
          0;  // libavcodec decides (one frame thread per hardware thread, capped at 16)
    }
    holes_.clear();
    // Deliberately no `skip_frame = AVDISCARD_NONKEY` for nearest-keyframe mode: skipped frames
    // do not advance the reorder buffer, so a keyframe only leaves the decoder when the *next*
    // keyframe arrives, which makes a long GOP dramatically slower rather than faster.

    if (hw != nullptr) {
      AVBufferRef* dev = nullptr;
      const char* device = opt_.hardware.device.empty() ? nullptr : opt_.hardware.device.c_str();
      if (int r = av_hwdevice_ctx_create(&dev, hw->type, device, nullptr, 0); r < 0) {
        return fail(ErrorCode::hardware_unavailable, r,
                    std::string("av_hwdevice_ctx_create(") + av_hwdevice_get_type_name(hw->type) +
                        (device ? std::string(", \"") + device + "\")" : ")"));
      }
      hw_device_.reset(dev);
      hw_type_ = hw->type;
      hw_state_ = std::make_unique<HwState>();
      hw_state_->hw_pix_fmt = hw->pix_fmt;
      cc->hw_device_ctx = av_buffer_ref(hw_device_.get());
      if (cc->hw_device_ctx == nullptr)
        return fail(ErrorCode::out_of_memory, "av_buffer_ref(hw_device_ctx)");
      cc->opaque = hw_state_.get();
      cc->get_format = &hw_get_format;
      // Surfaces the pipeline can hold at once beyond the decoder's own needs: held, pending, the
      // corrupt stash and the probe frame.
      cc->extra_hw_frames = 4;
      hw_state_->extra_frames = 0;  // avcodec_get_hw_frames_parameters already adds extra_hw_frames
    }

    if (int r = avcodec_open2(cc.get(), codec_desc_, nullptr); r < 0) {
      return fail(hw != nullptr ? ErrorCode::hardware_unavailable : ErrorCode::decoder_open_failed,
                  r, std::string("avcodec_open2(") + codec_desc_->name + ")");
    }
    if (stream_.synthesize_timestamps && cc->framerate.num > 0 && cc->framerate.den > 0) {
      stream_.frame_duration_hint =
          av_rescale_q(1, AVRational{cc->framerate.den, cc->framerate.num}, stream_.time_base);
      stream_.avg_frame_rate = cc->framerate;
    }
    if (stream_.synthesize_timestamps && stream_.frame_duration_hint <= 0) {
      stream_.frame_duration_hint =
          av_rescale_q(1, AVRational{1, 25}, stream_.time_base);  // libav's own default
      stream_.avg_frame_rate = AVRational{25, 1};
    }
    codec_ = std::move(cc);
    hw_active_ = hw != nullptr;
    hw_frames_seen_ = 0;
    return {};
  }

  /// HardwarePolicy::automatic: hardware only where it beats multi-threaded software for this
  /// seek-heavy access pattern. Every seek re-initialises the decoder session and every frame is
  /// downloaded, so hardware only pays for the more expensive codecs at larger sizes; software wins
  /// H.264 at every size. The thresholds below were chosen from measurement on the author's machine
  /// — a starting point, not a portable truth; tune them for yours.
  [[nodiscard]] static bool hardware_worthwhile(const AVCodecParameters& par,
                                                const AVCodec& codec) noexcept {
    const std::int64_t pixels =
        static_cast<std::int64_t>(std::max(par.width, 0)) * std::max(par.height, 0);
    switch (codec.id) {
      case AV_CODEC_ID_HEVC:
      case AV_CODEC_ID_AV1:
      case AV_CODEC_ID_VP9:
      case AV_CODEC_ID_VVC:
        return pixels >= static_cast<std::int64_t>(1280) * 720;
      default:
        return false;
    }
  }

  /// Tries the hardware candidates in preference order, then software. Each attempt is validated
  /// by decoding the first frame so lazy failures surface here, not on the first request.
  [[nodiscard]] std::expected<void, Error> setup_decoder() {
    std::string reason;
    Options effective = opt_;
    if (effective.hardware.policy == HardwarePolicy::automatic) {
      effective.hardware.policy = hardware_worthwhile(*st_->codecpar, *codec_desc_)
                                      ? HardwarePolicy::prefer_hardware
                                      : HardwarePolicy::software_only;
    }
    std::vector<HwCandidate> candidates = hw_candidates(codec_desc_, effective, reason);
    if (opt_.hardware.policy == HardwarePolicy::automatic &&
        effective.hardware.policy == HardwarePolicy::software_only) {
      reason = "automatic policy: software decoding is faster for " +
               std::to_string(st_->codecpar->width) + "x" + std::to_string(st_->codecpar->height) +
               " " + codec_desc_->name;
    }
    if (!candidates.empty() && !stream_.io_seekable) {
      // Probing a candidate decodes the first frame and a rejected one needs the input rewound;
      // a pipe cannot be rewound, so the probe would consume it. Software needs exactly one pass.
      candidates.clear();
      reason = "non-rewindable input: hardware probing would consume it";
    }
    std::string attempts;
    for (const HwCandidate& c : candidates) {
      auto built = build_codec(&c);
      std::expected<void, Error> probed =
          built ? probe_first_frame() : std::expected<void, Error>{};
      if (built && probed && hw_state_ != nullptr && !hw_state_->declined &&
          hw_state_->got_hw_format) {
        set_active(ActiveDecoder{codec_desc_->name, true, from_av(c.type), hw_type_name(c.type),
                                 opt_.hardware.device, ""});
        return {};
      }
      const std::string why =
          !built ? built.error().message
          : !probed
              ? probed.error().message
              : std::string("decoder declined the hardware pixel format (profile not supported)");
      if (!attempts.empty()) attempts += "; ";
      attempts += hw_type_name(c.type) + ": " + why;
    }
    if (opt_.hardware.policy == HardwarePolicy::require_hardware) {
      return fail(ErrorCode::hardware_unavailable,
                  "no hardware decoder could be used (" + (attempts.empty() ? reason : attempts) +
                      ")");
    }
    if (auto r = build_codec(nullptr); !r) return r;
    if (auto r = probe_first_frame(); !r) return r;
    set_active(ActiveDecoder{codec_desc_->name, false, std::nullopt, "", "",
                             attempts.empty() ? reason : attempts});
    return {};
  }

  [[nodiscard]] static std::string hw_type_name(AVHWDeviceType t) {
    const char* n = av_hwdevice_get_type_name(t);
    return n != nullptr ? std::string{n} : std::string{"unknown"};
  }

  /// Re-creates the active hardware decoder (same device type and format) and re-probes it.
  [[nodiscard]] std::expected<void, Error> rebuild_hardware() {
    if (!hw_active_ || hw_state_ == nullptr)
      return fail(ErrorCode::internal, "no hardware decoder to rebuild");
    HwCandidate c{hw_type_, hw_state_->hw_pix_fmt};
    if (auto r = build_codec(&c); !r) return r;
    if (auto p = probe_first_frame(); !p) return p;
    if (hw_state_ == nullptr || hw_state_->declined || !hw_state_->got_hw_format) {
      return fail(ErrorCode::hardware_unavailable, "rebuilt decoder declined the hardware format");
    }
    return {};
  }

  [[nodiscard]] std::expected<void, Error> rebuild_software(std::string reason) {
    auto r = build_codec(nullptr);
    if (!r) {
      broken_reason_ =
          "software decoder could not be rebuilt after a hardware failure: " + r.error().message;
      return r;
    }
    // Report the software decoder even if the probe below fails: an I/O error there leaves a
    // working software path whose first request simply failed.
    set_active(ActiveDecoder{codec_desc_->name, false, std::nullopt, "", "", reason});
    if (auto p = probe_first_frame(); !p) {
      set_active(
          ActiveDecoder{codec_desc_->name, false, std::nullopt, "", "",
                        std::move(reason) + "; probe after rebuild failed: " + p.error().message});
      return p;
    }
    return {};
  }

  /// Decodes the first frame of the stream (any tolerance) and remembers its timestamp. Always
  /// goes through the explicit start seek, so on containers whose seek does not land on
  /// keyframes (MPEG-TS) the "first frame" really is the first frame, for every candidate.
  [[nodiscard]] std::expected<void, Error> probe_first_frame() {
    CancelToken none;
    keyframe_only_ = false;
    if (positioned_) {
      if (auto r = seek_to_start(); !r) return r;
    } else {
      positioned_ = true;
      landed_at_start_ = true;
      seek_target_ = stream_.start_pts;
    }
    auto sel = select(stream_.start_pts, std::numeric_limits<std::int64_t>::min(),
                      std::numeric_limits<std::int64_t>::max(), none);
    if (!sel) {
      if (hw_active_)
        return fail(ErrorCode::hardware_unavailable, "first frame: " + sel.error().message);
      return std::unexpected(std::move(sel.error()));
    }
    if (hw_active_ && sel->frame->format != hw_state_->hw_pix_fmt) {
      return fail(ErrorCode::hardware_unavailable, "decoder produced a software frame");
    }
    first_frame_ts_ = frame_ts(*sel->frame);
    probe_frame_ = sel->frame;
    if (st_->start_time == k::no_pts && fmt_->start_time == k::no_pts) {
      stream_.start_pts = first_frame_ts_;  // raw streams: anchor Time::zero() at the first frame
    }
    return {};
  }

  void set_active(ActiveDecoder a) {
    a.decoder_threads = codec_ ? codec_->thread_count : 0;
    std::lock_guard lk(active_mutex_);
    active_ = std::move(a);
  }

  [[nodiscard]] AVRational effective_sar() const noexcept {
    if (stream_.container_sar.num > 0 && stream_.container_sar.den > 0)
      return stream_.container_sar;
    if (probe_frame_ != nullptr && probe_frame_->sample_aspect_ratio.num > 0 &&
        probe_frame_->sample_aspect_ratio.den > 0) {
      return probe_frame_->sample_aspect_ratio;
    }
    if (stream_.codec_sar.num > 0 && stream_.codec_sar.den > 0) return stream_.codec_sar;
    return AVRational{1, 1};
  }

  void fill_info() {
    info_ = AssetInfo{};
    info_.container_name =
        fmt_->iformat != nullptr && fmt_->iformat->name != nullptr ? fmt_->iformat->name : "";
    info_.codec_name = codec_desc_->name;
    const auto src_fmt = static_cast<AVPixelFormat>(st_->codecpar->format);
    const char* fmt_name = av_get_pix_fmt_name(src_fmt);
    if (fmt_name == nullptr && probe_frame_ != nullptr) {
      fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(probe_frame_->format));
    }
    info_.source_pixel_format = fmt_name != nullptr ? fmt_name : "";
    info_.video_stream_index = stream_.index;
    info_.time_base = from_av(stream_.time_base);
    info_.average_frame_rate =
        stream_.avg_frame_rate.num > 0 ? from_av(stream_.avg_frame_rate) : Rational{0, 1};
    if (stream_.duration_pts) {
      info_.duration = Time::from_timestamp(*stream_.duration_pts, info_.time_base);
    }
    if (st_->nb_frames > 0) info_.frame_count = st_->nb_frames;
    Size coded{st_->codecpar->width, st_->codecpar->height};
    if (probe_frame_ != nullptr && probe_frame_->width > 0)
      coded = Size{probe_frame_->width, probe_frame_->height};
    info_.coded_size = coded;
    const AVRational sar = effective_sar();
    const int rot = opt_.apply_preferred_track_transform ? stream_.transform.rotation : 0;
    info_.display_size = display_size(coded, sar, opt_.apply_sample_aspect_ratio, rot);
    info_.output_size = converter_->output_size(coded, sar);
    info_.rotation_degrees = stream_.transform.rotation;
    info_.mirrored = stream_.transform.mirrored;
    info_.sample_aspect_ratio = from_av(sar);
    info_.seekable = stream_.seekable;
    info_.timestamps_synthesized = stream_.synthesize_timestamps;
    const auto name_or_empty = [](const char* n) {
      return n != nullptr ? std::string{n} : std::string{};
    };
    const AVCodecParameters& par = *st_->codecpar;
    info_.color_transfer = par.color_trc != AVCOL_TRC_UNSPECIFIED
                               ? name_or_empty(av_color_transfer_name(par.color_trc))
                               : std::string{};
    info_.color_primaries = par.color_primaries != AVCOL_PRI_UNSPECIFIED
                                ? name_or_empty(av_color_primaries_name(par.color_primaries))
                                : std::string{};
    info_.color_space = par.color_space != AVCOL_SPC_UNSPECIFIED
                            ? name_or_empty(av_color_space_name(par.color_space))
                            : std::string{};
  }

  // Positioning. A seek is a hint: where it landed is established from the first keyframe packet
  // after it, before anything is decoded. Containers with a trusted index (MP4, Matroska) correct
  // an overshoot with one re-seek to the previous keyframe; containers without one (MPEG-TS) aim a
  // GOP early and scan packets up to the target, recording every keyframe (time, byte position, GOP
  // extent) in a lazily built index that later requests position from with one byte seek. Decoding
  // verifies the landing again, for demuxers whose keyframe flags cannot be trusted.

  /// A keyframe packet seen on a container without a trusted index: where it is (byte position for
  /// AVSEEK_FLAG_BYTE) and, once the packets after it were read contiguously up to the next
  /// keyframe, where its GOP ends. Sorted by pts.
  struct KeyEntry {
    std::int64_t pts;
    std::int64_t dts;
    std::int64_t pos;
    std::int64_t next_pts;  ///< k::no_pts = unknown; INT64_MAX = the stream ended inside this GOP
  };
  static constexpr std::size_t no_entry = static_cast<std::size_t>(-1);
  static constexpr std::size_t max_key_entries = 1u << 20;
  /// Scanned packets of the chosen GOP are kept for replay up to this much; beyond it (4K at high
  /// bit rates) the pipeline goes back to the keyframe with a byte seek instead.
  static constexpr std::size_t max_gop_buffer_bytes = 64u << 20;

  void reset_position() noexcept {
    av_frame_unref(recv_.get());
    av_frame_unref(held_.get());
    av_frame_unref(pending_.get());
    av_frame_unref(corrupt_last_.get());
    held_valid_ = pending_valid_ = corrupt_valid_ = false;
    held_concealed_ = pending_concealed_ = false;
    tainted_ = false;
    draining_ = eof_ = drained_ = false;
    landing_known_ = false;
    last_received_ts_ = k::no_pts;
    last_fed_dts_ = last_fed_pts_ = k::no_pts;
    last_key_ts_ = k::no_pts;
    last_end_ = std::numeric_limits<std::int64_t>::min();
    received_since_seek_ = 0;
    synth_ts_ = k::no_pts;
    demux_errors_ = decode_errors_ = 0;
    probe_frame_ = nullptr;
    contig_key_ = no_entry;
    // Nothing survives a reposition, so no gap recorded before it can matter.
    holes_.clear();
    clear_gop_buffer();
    if (pkt_pending_ && pkt_) av_packet_unref(pkt_.get());
    pkt_pending_ = false;
    if (land_pkt_) av_packet_unref(land_pkt_.get());
  }

  [[nodiscard]] int raw_seek(std::int64_t ts) noexcept {
    ++cur_.seeks;
    // min_ts = INT64_MIN, ts = max_ts: "the keyframe at or before ts", never later.
    return avformat_seek_file(fmt_.get(), stream_.index, std::numeric_limits<std::int64_t>::min(),
                              ts, ts, 0);
  }

  void after_seek(std::int64_t target, bool at_start) noexcept {
    avcodec_flush_buffers(codec_.get());
    reset_position();
    awaiting_key_ = true;
    positioned_ = true;
    landed_at_start_ = at_start;
    seek_target_ = target;
    synth_ts_ = target;
  }

  /// Seeks towards `target`. Where the seek actually landed is established by read_landing().
  [[nodiscard]] std::expected<void, Error> seek_to(std::int64_t target) {
    const int r = raw_seek(target);
    if (r < 0) {
      // The demuxer may have moved; nothing held is trustworthy any more.
      reset_position();
      positioned_ = false;
      return fail(ErrorCode::seek_failed, r, "avformat_seek_file");
    }
    after_seek(target, false);
    return {};
  }

  /// Positions on a recorded keyframe packet by byte offset (containers without a trusted index).
  /// The next packet read is that keyframe.
  [[nodiscard]] std::expected<void, Error> byte_seek(const KeyEntry& e) {
    ++cur_.seeks;
    const int r = av_seek_frame(fmt_.get(), -1, e.pos, AVSEEK_FLAG_BYTE);
    if (r < 0) {
      reset_position();
      positioned_ = false;
      return fail(ErrorCode::seek_failed, r, "av_seek_frame(AVSEEK_FLAG_BYTE)");
    }
    after_seek(e.pts, false);
    return {};
  }

  /// Positions on the very first packet, unconditionally. A timestamp below the stream's first
  /// DTS resolves to the first packet in libavformat's binary search (MPEG-TS/PS), to sample 0 in
  /// mov and to the first cue in Matroska; index-based demuxers that reject an out-of-range
  /// timestamp get a plain seek to start_pts, and if that fails too the input is re-opened.
  [[nodiscard]] std::expected<void, Error> seek_to_start() {
    if (!stream_.seekable) {
      if (!stream_.io_seekable) {
        return fail(ErrorCode::not_seekable, "the source cannot be rewound");
      }
      return reopen();
    }
    const std::int64_t margin = av_rescale_q(10, AVRational{1, 1}, stream_.time_base);
    const std::int64_t floor_ts = std::numeric_limits<std::int64_t>::min() / 2;
    const std::int64_t early =
        stream_.start_pts - margin < floor_ts ? floor_ts : stream_.start_pts - margin;
    int r = raw_seek(early);
    if (r < 0 && r != k::exit_requested) r = raw_seek(stream_.start_pts);
    if (r < 0) {
      reset_position();
      positioned_ = false;
      if (r == k::exit_requested)
        return fail(ErrorCode::seek_failed, r, "avformat_seek_file(start)");
      if (!stream_.io_seekable) {
        return fail(ErrorCode::not_seekable, r,
                    "seeking to the start failed and the source cannot be re-opened");
      }
      return reopen();
    }
    after_seek(stream_.start_pts, true);
    return {};
  }

  /// Re-opens the input from scratch (non-seekable sources that must rewind, or after a failed
  /// seek). The re-probe runs its I/O through the interrupt callback, so under a cancelled request
  /// it keeps only what it managed to read: a duration short of the real one would then reject
  /// legitimate times for the life of the generator. Hence no re-open under an already-cancelled
  /// request, and the old duration is kept when a cancellation lands mid-probe.
  [[nodiscard]] std::expected<void, Error> reopen() {
    if (current_token_ != nullptr && current_token_->requested()) {
      return fail(ErrorCode::cancelled, k::exit_requested,
                  "cancelled before re-opening the source");
    }
    const bool was_hw = hw_active_ && hw_state_ != nullptr && hw_device_ != nullptr;
    HwCandidate hw{};
    if (was_hw) {
      hw.pix_fmt = hw_state_->hw_pix_fmt;
      hw.type = hw_type_;
    }
    const std::int64_t start_pts = stream_.start_pts;
    const std::optional<std::int64_t> duration = stream_.duration_pts;
    const bool had_stream = st_ != nullptr;
    codec_.reset();
    fmt_.reset();
    st_ = nullptr;
    positioned_ = false;
    auto r = open_input();
    if (r) r = build_codec(was_hw ? &hw : nullptr);
    if (!r) {
      // The old context is gone and the new one failed; image_at() retries on the next request.
      codec_.reset();
      fmt_.reset();
      st_ = nullptr;
      broken_reason_ = r.error().message;
      return fail(ErrorCode::seek_failed, r.error().av_error,
                  "re-opening the source: " + r.error().message);
    }
    tail_end_ = k::no_pts;  // a re-opened (or grown) source may reach further than it did
    verified_to_ = k::no_pts;
    if (had_stream) {
      stream_.start_pts = start_pts;  // keep the time origin established at open()
      // And the duration: an interrupted probe under-reports it, never over-reports it, so a
      // longer answer is a source that genuinely grew and a shorter one is a truncated read.
      if (duration && (!stream_.duration_pts || *stream_.duration_pts < *duration))
        stream_.duration_pts = duration;
    }
    positioned_ = true;
    awaiting_key_ = true;
    landed_at_start_ = true;
    seek_target_ = stream_.start_pts;
    return {};
  }

  /// An interrupt that fired inside libavformat I/O leaves the AVIOContext refusing to read. Clear
  /// it and forget the position: the next request seeks (which resets the I/O layer properly) or,
  /// on a pipe, continues from where the read stopped.
  ///
  /// The state left behind is not reliably `error == AVERROR_EXIT` — the MPEG-TS demuxer turns the
  /// short read into AVERROR_EOF, and a later seek clears `error` but not `eof_reached`, which on
  /// its own is indistinguishable from a genuine EOF. So the interrupt is recorded when it fires
  /// (interrupt_callback) rather than inferred here, and consumed either way.
  ///
  /// Returns false when the sticky state could not be cleared in place -- a libavformat major
  /// tryResetIoState() was never verified against -- and the source can be re-opened. The caller
  /// must then re-establish it before the next read; nothing else clears the I/O layer.
  [[nodiscard]] bool recover_after_interrupt() noexcept {
    const bool fired = std::exchange(interrupt_fired_, false);
    if (!fmt_ || fmt_->pb == nullptr) return true;
    if (fmt_->pb->error != k::exit_requested && !(fired && fmt_->pb->eof_reached != 0)) return true;
    const bool cleared = tryResetIoState(*fmt_->pb);
    const std::int64_t last = last_received_ts_;
    avformat_flush(fmt_.get());
    if (codec_) avcodec_flush_buffers(codec_.get());
    reset_position();
    if (!cleared && stream_.io_seekable) {
      positioned_ = false;  // nothing here can unstick the I/O layer; the caller re-opens
      return false;
    }
    if (stream_.seekable) {
      positioned_ = false;  // forces a seek in position_for()
    } else {
      // Best effort on a non-rewindable input: the demuxer is somewhere at or after the last frame.
      positioned_ = true;
      landed_at_start_ = false;
      last_received_ts_ = last;
    }
    return true;
  }

  /// Re-positions for `P` after an end of stream that produced no frames at all. libavio's sticky
  /// end-of-file is cleared first: a seek leaves it set, so the retry would read EOF again. On a
  /// libavformat major where it cannot be cleared in place, the source is re-opened instead.
  [[nodiscard]] std::expected<void, Error> retry_after_empty_eof(std::int64_t P,
                                                                 const CancelToken& token) {
    if (fmt_ && fmt_->pb != nullptr && !tryResetIoState(*fmt_->pb) && stream_.io_seekable) {
      // An unverified libavformat major: the sticky end-of-file survives a seek, so the source is
      // re-established rather than retried in place. Slower by one re-open, and correct.
      reset_position();
      if (auto r = reopen(); !r) return r;
      return position_for(P, keyframe_only_, token);
    }
    avformat_flush(fmt_.get());
    if (codec_) avcodec_flush_buffers(codec_.get());
    reset_position();
    positioned_ = false;  // forces a real seek rather than a decode-forward
    return position_for(P, keyframe_only_, token);
  }

  /// Seeks towards `target`, with the failure policy: interrupted I/O is reported as is, other
  /// failures count towards giving up on seeking and fall back to a re-open.
  [[nodiscard]] std::expected<void, Error> seek_or_reopen(std::int64_t target) {
    if (target <= stream_.start_pts) return seek_to_start();
    auto r = seek_to(target);
    if (r) {
      seek_failures_ = 0;
      return {};
    }
    if (r.error().av_error == k::exit_requested) return r;
    if (++seek_failures_ >= max_seek_failures) stream_.seekable = false;
    if (!stream_.io_seekable) {
      return fail(ErrorCode::not_seekable, r.error().av_error,
                  "seeking failed and the source cannot be re-opened");
    }
    return reopen();
  }

  [[nodiscard]] std::int64_t one_second() const noexcept {
    return av_rescale_q(1, AVRational{1, 1}, stream_.time_base);
  }
  [[nodiscard]] std::int64_t frames_ticks(int n) const noexcept {
    return stream_.frame_duration_hint > 0 ? n * stream_.frame_duration_hint : 0;
  }

  /// Decides between continuing to decode forward and seeking, then positions accordingly. In
  /// keyframe mode the answer is a keyframe packet that is then decoded alone
  /// (arm_keyframe_decode).
  [[nodiscard]] std::expected<void, Error> position_for(std::int64_t P, bool keyframe_mode,
                                                        const CancelToken& token) {
    backoffs_ = 0;
    backoff_step_ = 0;
    // Continuing forward answers from frames the decoder has yet to produce, so it is only sound
    // while every frame between the decoder's frontier and P will actually come out: a gap there
    // (a non-reference frame skipped for an abandoned request) would be the frame on screen.
    const bool forward_ok = positioned_ && !drained_ && !eof_ && last_received_ts_ != k::no_pts &&
                            P >= last_received_ts_ && !hole_in(last_received_ts_, P);
    if (!stream_.seekable) {
      if (forward_ok) return {};
      if (positioned_ && !drained_ && !eof_ && last_received_ts_ == k::no_pts && landed_at_start_)
        return {};
      if (!stream_.io_seekable) {
        return fail(
            ErrorCode::not_seekable,
            "the source is not seekable; requested times must not precede the current position");
      }
      return reopen();
    }
    if (!keyframe_mode) {
      if (forward_ok && forward_is_cheaper(P)) return {};
      if (positioned_ && !drained_ && !eof_ && last_received_ts_ == k::no_pts &&
          seek_target_ <= P && P - seek_target_ <= forward_scan_limit()) {
        return {};  // freshly positioned just before the target; the landing is verified in
                    // select()
      }
    }
    if (stream_.index_trusted) return position_indexed(P, keyframe_mode, token);
    return position_scanned(P, keyframe_mode, token);
  }

  /// Containers with a trusted index (MP4, Matroska): seek to the target, read the landing
  /// keyframe packet, and correct an overshoot (fragmented MP4, open-GOP leading pictures) with a
  /// re-seek to the previous keyframe. Fragmented MP4 is learned: its demuxer applies no reorder
  /// shift, so later seek targets carry it (`seek_bias_`) and land right the first time.
  [[nodiscard]] std::expected<void, Error> position_indexed(std::int64_t P, bool keyframe_mode,
                                                            const CancelToken& token) {
    std::int64_t target = P - seek_bias_;
    bool need_seek = true;
    for (int round = 0;; ++round) {
      if (need_seek) {
        if (auto r = seek_or_reopen(target); !r) return r;
      }
      need_seek = true;
      if (!stream_.seekable) return {};  // gave up on seeking: the re-open positioned at the start
      auto land = read_landing(P, /*scan=*/false, keyframe_mode, token);
      if (!land) return std::unexpected(std::move(land.error()));
      if (land->found || landed_at_start_ || !key_flags_reliable_) return {};
      if (land->first_key_pts == k::no_pts) {
        // No keyframe packet before the end: the seek landed in the tail. back_off() seeks itself.
        if (auto s = back_off(P, k::no_pts); !s) return s;
        need_seek = false;
        continue;
      }
      // Overshoot: the landing keyframe is past P. One cheap seek to the keyframe strictly before
      // it.
      const std::int64_t kp = land->first_key_pts;
      const std::int64_t kd = land->first_key_dts;
      const std::int64_t delay = (kd != k::no_pts && kp > kd) ? kp - kd : reorder_ticks_;
      if (kd != k::no_pts && reorder_ticks_ > 0 && kd > P - reorder_ticks_ && seek_bias_ == 0) {
        // The demuxer searched its DTS index with our PTS unshifted (fragmented MP4): remember the
        // reorder delay so the next seek aims right. Open-GOP overshoots (the CRA's DTS is already
        // below P - delay) must not set it, or every seek would land a GOP early.
        seek_bias_ = reorder_ticks_;
      }
      if (round >= 3 || backoffs_ >= max_backoffs)
        return seek_to_start_and_land(P, keyframe_mode, token);
      target = kp - 1 - delay -
               (round > 0 ? std::max({one_second(), gop_hint_, std::int64_t{1}}) * round : 0);
      ++backoffs_;
    }
  }

  [[nodiscard]] std::expected<void, Error> seek_to_start_and_land(std::int64_t P,
                                                                  bool keyframe_mode,
                                                                  const CancelToken& token) {
    if (auto r = seek_to_start(); !r) return r;
    auto land = read_landing(P, /*scan=*/!stream_.index_trusted, keyframe_mode, token);
    if (!land) return std::unexpected(std::move(land.error()));
    return {};
  }

  /// Containers without a trusted index (MPEG-TS): position with a byte seek when the keyframe
  /// covering P is already known; otherwise aim one GOP early, scan the packets up to the target
  /// and choose the last keyframe at or before it (recording every keyframe on the way).
  [[nodiscard]] std::expected<void, Error> position_scanned(std::int64_t P, bool keyframe_mode,
                                                            const CancelToken& token) {
    if (const KeyEntry* e = covering_key(P); e != nullptr) {
      if (auto r = byte_seek(*e); !r) return r;
      if (!keyframe_mode) return {};
      auto land = read_landing(P, /*scan=*/false, keyframe_mode, token);
      if (!land) return std::unexpected(std::move(land.error()));
      if (land->found) return {};
      // The recorded position no longer holds (the file changed): fall through to a fresh scan.
    }
    const std::int64_t margin = frames_ticks(2) + reorder_ticks_;
    std::int64_t aim = P - margin - (gop_hint_ > 0 ? gop_hint_ : 0);
    if (auto r = seek_or_reopen(aim); !r) return r;
    for (;;) {
      if (!stream_.seekable) return {};
      auto land = read_landing(P, /*scan=*/true, keyframe_mode, token);
      if (!land) return std::unexpected(std::move(land.error()));
      if (land->found || landed_at_start_ || !key_flags_reliable_) return {};
      // Landed after the keyframe covering P (or in the tail): retreat by a growing step.
      if (auto s = back_off(P, land->first_key_pts); !s) return s;
    }
  }

  /// Retreats after a landing proved to be past `P`. The new target is the observed keyframe (or
  /// the last target) minus a doubling step (starting at the GOP length, at least one second) and
  /// a reorder margin: the generic seek lands on the last packet with DTS <= target, and a
  /// keyframe's DTS precedes its PTS, so aiming exactly at a keyframe lands just after it.
  [[nodiscard]] std::expected<void, Error> back_off(std::int64_t P,
                                                    std::int64_t observed_key /* or k::no_pts */) {
    if (backoff_step_ <= 0) backoff_step_ = std::max({one_second(), gop_hint_, std::int64_t{1}});
    const std::int64_t margin = frames_ticks(2) + reorder_ticks_;
    const std::int64_t anchor =
        std::min(P, observed_key != k::no_pts ? observed_key : seek_target_);
    std::int64_t earlier = anchor - backoff_step_ - margin;
    if (earlier >= seek_target_)
      earlier = seek_target_ - backoff_step_;  // always strictly earlier than last time
    if (backoff_step_ < std::numeric_limits<std::int64_t>::max() / 2) backoff_step_ *= 2;
    if (++backoffs_ >= max_backoffs || earlier <= stream_.start_pts) return seek_to_start();
    return seek_or_reopen(earlier);
  }

  /// Without a trusted index, decode forward when the target is within one GOP (at least 3 s):
  /// a seek would land in the same or the next GOP and decode about as many frames anyway.
  [[nodiscard]] std::int64_t forward_scan_limit() const noexcept {
    const std::int64_t three_seconds = av_rescale_q(3, AVRational{1, 1}, stream_.time_base);
    return std::max(three_seconds, gop_hint_);
  }

  /// The index entry of the keyframe at or before P on a container with a trusted index, or
  /// nullptr when the index does not cover P (a fragmented MP4 whose later fragments have not been
  /// read yet). The mov index is in the DTS domain; `reorder_ticks_` (a keyframe's pts - dts,
  /// learned from the first keyframe packet) moves P there. Matroska cues are in the PTS domain and
  /// its packets carry no DTS, so the shift is zero.
  [[nodiscard]] const AVIndexEntry* index_key_before(std::int64_t P) const noexcept {
    const int n = avformat_index_get_entries_count(st_);
    if (n <= 0) return nullptr;
    const std::int64_t shift = index_shift();
    const std::int64_t ts = P > std::numeric_limits<std::int64_t>::min() + shift ? P - shift : P;
    int e = av_index_search_timestamp(st_, ts, AVSEEK_FLAG_BACKWARD);
    if (e < 0) return nullptr;
    if (e == n - 1) {
      const AVIndexEntry* last = avformat_index_get_entry(st_, e);
      if (last == nullptr ||
          ts > last->timestamp + std::max<std::int64_t>(2 * stream_.frame_duration_hint, 1))
        return nullptr;
    }
    for (; e >= 0; --e) {
      const AVIndexEntry* entry = avformat_index_get_entry(st_, e);
      if (entry == nullptr) return nullptr;
      if ((entry->flags & AVINDEX_KEYFRAME) != 0) return entry;
    }
    return nullptr;
  }

  /// True when decoding forward to `target` is cheaper than seeking: the keyframe at or before the
  /// target has already been fed to the decoder (both sides in the container's own timestamp
  /// domain), or the distance is short. Cost-aware for slow seeks (hardware): frames between the
  /// current position and that keyframe are decoded forward while they cost less than a seek.
  [[nodiscard]] bool forward_is_cheaper(std::int64_t target) const noexcept {
    std::int64_t key_ts = k::no_pts;  // the covering keyframe, in the same domain as fed_ts
    std::int64_t fed_ts = k::no_pts;
    bool covered = false;
    if (stream_.index_trusted) {
      if (const AVIndexEntry* kf = index_key_before(target); kf != nullptr) {
        key_ts = kf->timestamp;
        fed_ts = last_fed_index_ts();
        covered = true;
      }
    } else if (const KeyEntry* e = covering_key(target); e != nullptr) {
      key_ts = e->dts != k::no_pts ? e->dts : e->pts;
      fed_ts = e->dts != k::no_pts ? last_fed_dts_ : last_fed_pts_;
      covered = true;
    }
    if (covered && key_ts != k::no_pts && fed_ts != k::no_pts) {
      // The covering keyframe has been fed: the target is downstream of the decoder's state.
      if (key_ts <= fed_ts + std::max<std::int64_t>(stream_.frame_duration_hint, 0)) return true;
      // A keyframe lies ahead: forward wins while the frames up to it cost less than a seek (a
      // real trade on hardware decoders and tiny frames).
      if (frame_cost_ms_ > 0 && stream_.frame_duration_hint > 0) {
        const double frames_to_key =
            static_cast<double>(key_ts - fed_ts) / static_cast<double>(stream_.frame_duration_hint);
        return frames_to_key * frame_cost_ms_ < seek_cost_ms_;
      }
      return false;
    }
    if (stream_.index_trusted)
      return target - last_received_ts_ <=
             frames_ticks(
                 4);  // the index does not cover P (unread fragment): seeks are exact there
    return target - last_received_ts_ <= forward_scan_limit();
  }

  [[nodiscard]] std::size_t key_lower_bound(std::int64_t pts) const noexcept {
    std::size_t lo = 0, hi = key_index_.size();
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      if (key_index_[mid].pts < pts)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  /// Records a keyframe packet; links it to the previous keyframe when the packets in between were
  /// read contiguously (so that keyframe's GOP extent becomes known).
  void record_key(std::int64_t pts, std::int64_t dts, std::int64_t pos) {
    if (pts == k::no_pts || pos < 0 || stream_.index_trusted) return;
    std::size_t i = key_lower_bound(pts);
    if (i < key_index_.size() && key_index_[i].pts == pts) {
      key_index_[i].dts = dts;
      key_index_[i].pos = pos;
    } else {
      if (key_index_.size() >= max_key_entries) return;
      key_index_.insert(key_index_.begin() + static_cast<std::ptrdiff_t>(i),
                        KeyEntry{pts, dts, pos, k::no_pts});
      if (contig_key_ != no_entry && contig_key_ >= i) ++contig_key_;
    }
    if (contig_key_ != no_entry && contig_key_ < key_index_.size() &&
        key_index_[contig_key_].pts < pts) {
      key_index_[contig_key_].next_pts = pts;
      gop_hint_ = std::max(gop_hint_, pts - key_index_[contig_key_].pts);
    }
    contig_key_ = i;
  }

  /// The recorded keyframe whose GOP is known to contain P, or nullptr.
  [[nodiscard]] const KeyEntry* covering_key(std::int64_t P) const noexcept {
    if (key_index_.empty()) return nullptr;
    std::size_t i = key_lower_bound(P);
    if (i == key_index_.size() || key_index_[i].pts != P) {
      if (i == 0) return nullptr;
      --i;
    }
    const KeyEntry& e = key_index_[i];
    if (e.pts > P || e.next_pts == k::no_pts || P >= e.next_pts) return nullptr;
    return &e;
  }

  /// True when the container index records a keyframe in (key_pts, P], i.e. `key_pts` is probably
  /// not the keyframe covering P. The DTS index is shifted by the *smallest* reorder delay, so an
  /// open-GOP I-frame may be placed a frame early: a true result only makes read_landing scan on.
  [[nodiscard]] bool index_has_key_between(std::int64_t key_pts, std::int64_t P) const noexcept {
    if (!stream_.index_trusted) return false;
    const AVIndexEntry* kf = index_key_before(P);
    if (kf == nullptr) return false;
    const std::int64_t kf_pts = kf->timestamp + index_shift();
    return kf_pts > key_pts && kf_pts <= P;
  }

  /// Whether `key_pts` is the keyframe at or before P with no other keyframe in between, known
  /// from the container index (MP4/Matroska) or the recorded keyframe index (MPEG-TS).
  [[nodiscard]] bool key_covers(std::int64_t key_pts, std::int64_t P) const noexcept {
    if (key_pts > P) return false;
    if (stream_.index_trusted) {
      const AVIndexEntry* kf = index_key_before(P);
      if (kf == nullptr) return false;
      // mov indexes DTS (a keyframe's pts is its dts plus the reorder delay); Matroska cues are
      // PTS.
      return kf->timestamp + index_shift() == key_pts;
    }
    const KeyEntry* e = covering_key(P);
    return e != nullptr && e->pts == key_pts;
  }

  /// Offset from the container index's timestamp domain to presentation time: the reorder delay
  /// when the index (and the keyframe packets) carry DTS (mov), zero for a PTS index (Matroska).
  [[nodiscard]] std::int64_t index_shift() const noexcept {
    return keys_have_dts_ ? reorder_ticks_ : 0;
  }
  /// The last fed packet's timestamp in the container index's domain.
  [[nodiscard]] std::int64_t last_fed_index_ts() const noexcept {
    return keys_have_dts_ ? last_fed_dts_ : last_fed_pts_;
  }

  [[nodiscard]] std::int64_t frame_ts(const AVFrame& f) const noexcept {
    if (f.best_effort_timestamp != k::no_pts) return f.best_effort_timestamp;
    if (f.pts != k::no_pts) return f.pts;
    if (f.pkt_dts != k::no_pts) return f.pkt_dts;
    return synth_ts_ != k::no_pts ? synth_ts_ : stream_.start_pts;
  }

  [[nodiscard]] std::int64_t frame_duration(const AVFrame& f) const noexcept {
    return f.duration > 0 ? f.duration : stream_.frame_duration_hint;
  }

  void hold_received(bool concealed) noexcept {
    av_frame_unref(held_.get());
    av_frame_move_ref(held_.get(), recv_.get());
    held_valid_ = true;
    held_concealed_ = concealed;
    av_frame_unref(pending_.get());
    pending_valid_ = false;
  }

  void pend_received(bool concealed) noexcept {
    av_frame_unref(pending_.get());
    av_frame_move_ref(pending_.get(), recv_.get());
    pending_valid_ = true;
    pending_concealed_ = concealed;
  }

  /// Pulls one frame. Returns 0 (frame in recv_), k::eof, k::eagain (needs a packet) or an error.
  [[nodiscard]] int receive_one() noexcept {
    av_frame_unref(recv_.get());
    const int r = avcodec_receive_frame(codec_.get(), recv_.get());
    if (r == 0) {
      ++cur_.frames_decoded;
    }
    if (r == k::eagain && draining_) return k::eof;
    return r;
  }

  /// Learns the stream's reorder delay (a keyframe's pts - dts; the smallest seen, so open-GOP CRAs
  /// with leading pictures do not inflate it) and remembers the stream position of every keyframe
  /// packet on containers without a trusted index.
  void note_packet(const AVPacket& p) {
    const bool key = (p.flags & AV_PKT_FLAG_KEY) != 0;
    if (!key) return;
    if (p.pts != k::no_pts && p.dts != k::no_pts && p.pts >= p.dts) {
      const std::int64_t d = p.pts - p.dts;
      reorder_ticks_ = reorder_known_ ? std::min(reorder_ticks_, d) : d;
      reorder_known_ = true;
      keys_have_dts_ = true;
    }
    record_key(p.pts, p.dts, p.pos);
  }

  /// Sends the packet held in pkt_. Returns 0 (consumed), k::eagain (kept for a retry after the
  /// decoder has been drained) or an error (packet dropped).
  [[nodiscard]] int send_held_packet() noexcept {
    const std::int64_t fed_dts = pkt_->dts;
    const std::int64_t fed_pts = pkt_->pts;
    const int s = avcodec_send_packet(codec_.get(), pkt_.get());
    if (s == k::eagain) {
      pkt_pending_ = true;  // the decoder wants a receive first; keep the packet
      return s;
    }
    if (s == 0) {
      if (fed_dts != k::no_pts) last_fed_dts_ = fed_dts;
      if (fed_pts != k::no_pts)
        last_fed_pts_ = last_fed_pts_ == k::no_pts ? fed_pts : std::max(last_fed_pts_, fed_pts);
    }
    av_packet_unref(pkt_.get());
    pkt_pending_ = false;
    return s;
  }

  /// Reads the next packet of our stream into pkt_ (other streams are dropped). Returns 0, k::eof
  /// (after a live-source growth check), k::exit_requested or a demux error (transient ones are
  /// skipped up to a limit).
  /// Not noexcept: the keyframe index this records into allocates. A std::bad_alloc here
  /// would be std::terminate rather than the ErrorCode::out_of_memory the API can report.
  [[nodiscard]] int read_video_packet(const CancelToken& token) {
    if (replaying_ && replay_pos_ < gop_buffer_.size()) {
      // Packets scanned past the chosen keyframe are replayed; the demuxer sits right after them.
      av_packet_unref(pkt_.get());
      av_packet_move_ref(pkt_.get(), gop_buffer_[replay_pos_].get());
      if (++replay_pos_ == gop_buffer_.size()) clear_gop_buffer();
      return 0;
    }
    for (;;) {
      if (token.requested()) return k::exit_requested;
      int r = av_read_frame(fmt_.get(), pkt_.get());
      if (r == k::exit_requested) return r;
      if (r == k::eof || (r < 0 && ++demux_errors_ > max_consecutive_errors)) return k::eof;
      if (r < 0) {
        tainted_ = true;
        continue;  // transient demux error: skip and keep reading
      }
      demux_errors_ = 0;
      if (pkt_->stream_index == stream_.index && pkt_->pts != k::no_pts) {
        verified_to_ = verified_to_ == k::no_pts ? pkt_->pts : std::max(verified_to_, pkt_->pts);
      }
      if (pkt_->stream_index != stream_.index) {
        av_packet_unref(pkt_.get());
        continue;
      }
      if (!first_packet_seen_) {
        // Trust the demuxer's keyframe flags only if it flags the very first packet; a demuxer
        // that never sets the flag must not make us skip.
        first_packet_seen_ = true;
        key_flags_reliable_ = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
      }
      note_packet(*pkt_);
      return 0;
    }
  }

  /// Keeps the packet in pkt_ for replay (read_landing scan). False when the cap is exceeded.
  [[nodiscard]] bool buffer_packet() {
    const std::size_t bytes = static_cast<std::size_t>(std::max(pkt_->size, 0));
    if (gop_buffer_bytes_ + bytes > max_gop_buffer_bytes) {
      av_packet_unref(pkt_.get());
      clear_gop_buffer();
      return false;
    }
    PacketPtr p{av_packet_alloc()};
    if (!p) {
      av_packet_unref(pkt_.get());
      clear_gop_buffer();
      return false;
    }
    av_packet_move_ref(p.get(), pkt_.get());
    gop_buffer_.push_back(std::move(p));
    gop_buffer_bytes_ += bytes;
    return true;
  }
  void clear_gop_buffer() noexcept {
    gop_buffer_.clear();
    gop_buffer_bytes_ = 0;
    replay_pos_ = 0;
    replaying_ = false;
  }

  /// Clears libavio's sticky end-of-file / error state so reading can continue after an interrupt
  /// inside a read. Poking `eof_reached` / `error` is not promised to keep the demuxer consistent,
  /// so it is confined to the libavformat majors it was verified against -- 60 (FFmpeg 6.1),
  /// 61 (7.1), 62 (8.0) and 63 (9.0), each one built and the suite run against it. On any other
  /// major it changes nothing and returns false; the caller re-establishes the source instead,
  /// which costs one re-open per interrupt recovery. That is the right trade against failing the
  /// build, which would stop every consumer -- including the ones that never cancel -- over a
  /// path they never reach.
  ///
  /// Why an unknown major cannot simply seek instead: a same-position avio_seek clears
  /// `eof_reached` but leaves `error` set -- `s->eof_reached = 0` on every exit path, `s->error`
  /// never assigned; read from source at n7.1.2 / n8.0 / n9.0.1 (avio_seek itself changed in 9,
  /// this property did not) and confirmed behaviourally at 60 by the suite. And avio_read returns
  /// `s->error` whenever it read nothing, while the case this function exists for is exactly
  /// `pb->error == AVERROR_EXIT` -- so seeking alone would leave every later read failing.
  [[nodiscard]] static bool tryResetIoState(AVIOContext& pb) noexcept {
#if LIBAVFORMAT_VERSION_MAJOR <= 63
    pb.eof_reached = 0;
    pb.error = 0;
    return true;
#else
    (void)pb;
    return false;
#endif
  }

  /// Result of read_landing().
  struct Landing {
    bool found{false};  ///< a keyframe packet at or before P is ready (pending or positioned)
    std::int64_t first_key_pts{k::no_pts};  ///< the first keyframe packet seen (> P when !found)
    std::int64_t first_key_dts{k::no_pts};
    bool eof{false};
  };

  /// Establishes where a seek landed from the packets, without decoding. `scan == false` (trusted
  /// index): the first keyframe packet is the landing; it is kept in pkt_ for the decoder when it
  /// is at or before P. `scan == true` (no index): keeps reading through the GOPs up to P, records
  /// every keyframe, and settles on the last keyframe at or before P — held in pkt_ for a
  /// keyframe-only decode, or reached again with a byte seek for an exact decode.
  [[nodiscard]] std::expected<Landing, Error> read_landing(std::int64_t P, bool scan,
                                                           bool keyframe_mode,
                                                           const CancelToken& token) {
    Landing L;
    std::int64_t best_pts = k::no_pts, best_dts = k::no_pts, best_pos = -1;
    bool best_held = false;  // keyframe mode: the best packet is parked in land_pkt_
    bool buffering = false;  // exact mode: packets after the best keyframe are kept for replay
    bool scanning = scan;    // becomes true on a trusted-index container whose seek undershot
    std::int64_t landing_pts = k::no_pts;  // first packet read after the seek
    // arm_keyframe_decode() passes P = INT64_MAX ("the next keyframe, wherever it is"): saturate.
    std::int64_t horizon = 0;
    if (__builtin_add_overflow(P, reorder_ticks_ + frames_ticks(1), &horizon)) {
      horizon = std::numeric_limits<std::int64_t>::max();
    }
    clear_gop_buffer();
    for (;;) {
      const int r = read_video_packet(token);
      if (r == k::exit_requested) {
        if (token.requested()) return fail(ErrorCode::cancelled, "cancelled");
        return fail(ErrorCode::decode_failed, r,
                    "av_read_frame: interrupted without a cancellation");
      }
      if (r == k::eof) {
        L.eof = true;
        break;
      }
      if (!key_flags_reliable_) {
        // Cannot tell keyframes apart at the packet level: feed everything, the decoder sorts it
        // out.
        pkt_pending_ = true;
        L.found = true;
        return L;
      }
      const bool key = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
      const std::int64_t pts =
          pkt_->pts != k::no_pts
              ? pkt_->pts
              : (pkt_->dts != k::no_pts ? pkt_->dts + reorder_ticks_ : k::no_pts);
      if (landing_pts == k::no_pts && pts != k::no_pts) landing_pts = pts;
      if (!key) {
        if (buffering) {
          // Part of the chosen keyframe's GOP: keep it instead of reading it twice.
          const bool past = pts != k::no_pts && pts > horizon;
          if (!buffer_packet()) buffering = false;  // over the cap: fall back to a byte seek
          if (scanning && best_pts != k::no_pts && past)
            break;  // every later packet is past P in decode order too
          continue;
        }
        // In keyframe mode the scan runs on to the next keyframe so the chosen one's GOP extent is
        // recorded and later requests inside it are answered from the held frame.
        if (scanning && !keyframe_mode && best_pts != k::no_pts && pts != k::no_pts &&
            pts > horizon) {
          av_packet_unref(pkt_.get());
          break;
        }
        av_packet_unref(pkt_.get());
        continue;
      }
      if (pts == k::no_pts) {
        pkt_pending_ = true;  // a keyframe without a timestamp: nothing to verify against
        L.found = true;
        return L;
      }
      if (L.first_key_pts == k::no_pts) {
        L.first_key_pts = pts;
        L.first_key_dts = pkt_->dts;
        // No keyframe between the landing and this one: the GOP is at least that long.
        if (landing_pts != k::no_pts && pts > landing_pts)
          gop_hint_ = std::max(gop_hint_, pts - landing_pts);
      }
      if (pts > P) {
        if (best_pts != k::no_pts) {
          // The keyframe before this one is the answer; this packet is the next in decode order.
          if (buffering && !buffer_packet())
            buffering = false;
          else if (!buffering)
            av_packet_unref(pkt_.get());
          break;
        }
        av_packet_unref(pkt_.get());
        return L;  // overshoot: nothing at or before P was seen
      }
      if (!scanning) {
        if (!index_has_key_between(pts, P)) {
          pkt_pending_ = true;  // the landing keyframe covers P
          L.found = true;
          return L;
        }
        // The index records a later keyframe at or before P (mov lands a GOP early on edit-list
        // files). Read on like a scan: a keyframe past P ends it with the last one at or before P,
        // so a wrong index entry cannot make us overshoot.
        scanning = true;
      }
      best_pts = pts;
      best_dts = pkt_->dts;
      best_pos = pkt_->pos;
      if (keyframe_mode) {
        av_packet_unref(land_pkt_.get());
        av_packet_move_ref(land_pkt_.get(), pkt_.get());
        best_held = true;
      } else {
        clear_gop_buffer();
        buffering = buffer_packet();  // the keyframe itself is the first packet to feed
      }
    }
    if (best_pts == k::no_pts) {
      clear_gop_buffer();
      return L;  // tail of the stream without a keyframe, or nothing at all
    }
    if (L.eof) {
      // The stream ended inside this GOP: its extent is known.
      const std::size_t i = key_lower_bound(best_pts);
      if (i < key_index_.size() && key_index_[i].pts == best_pts)
        key_index_[i].next_pts = std::numeric_limits<std::int64_t>::max();
    }
    L.found = true;
    if (keyframe_mode && best_held) {
      av_packet_unref(pkt_.get());
      av_packet_move_ref(pkt_.get(), land_pkt_.get());
      pkt_pending_ = true;
      return L;
    }
    if (buffering && !gop_buffer_.empty()) {
      // The chosen keyframe and its GOP are buffered: decoding replays them.
      awaiting_key_ = true;
      replaying_ = true;
      return L;
    }
    clear_gop_buffer();
    // GOP too large to buffer: go back to the keyframe by byte position (trusted-index containers
    // refuse byte seeks and re-seek by timestamp below).
    if (best_pos >= 0 && !stream_.index_trusted) {
      const std::size_t i = key_lower_bound(best_pts);
      if (i < key_index_.size() && key_index_[i].pts == best_pts) {
        if (auto r = byte_seek(key_index_[i]); !r) return std::unexpected(std::move(r.error()));
      } else {
        if (auto r = byte_seek(KeyEntry{best_pts, best_dts, best_pos, k::no_pts}); !r)
          return std::unexpected(std::move(r.error()));
      }
      L.found = true;
      return L;
    }
    // No byte position (unusual): re-seek by timestamp.
    if (auto r = seek_or_reopen(best_pts - reorder_ticks_); !r)
      return std::unexpected(std::move(r.error()));
    return L;
  }

  /// Keyframe mode after positioning: feed the pending keyframe packet and drain, so the decoder
  /// emits that one frame at once instead of after a pipeline's worth of packets (frame threads
  /// hold ~thread_count packets). The decoder must be flushed before it is fed again (`drained_`).
  [[nodiscard]] std::expected<void, Error> arm_keyframe_decode(const CancelToken& token) {
    if (!pkt_pending_) {
      // Positioned but not read yet: fetch the keyframe packet.
      auto land = read_landing(std::numeric_limits<std::int64_t>::max(), /*scan=*/false,
                               /*keyframe_mode=*/true, token);
      if (!land) return std::unexpected(std::move(land.error()));
      if (!pkt_pending_) return {};  // nothing to feed (EOF); select() reports it
    }
    if (!key_flags_reliable_ || pkt_->pts == k::no_pts)
      return {};  // cannot single out a keyframe: decode normally
    awaiting_key_ = false;
    const int s = send_held_packet();
    if (s < 0 && s != k::eagain)
      return fail(ErrorCode::decode_failed, s, "avcodec_send_packet (keyframe)");
    (void)avcodec_send_packet(codec_.get(), nullptr);
    draining_ = true;
    drained_ = true;
    landing_known_ = true;
    return {};
  }

  /// Reads the next packet of our stream and feeds it. Returns 0, k::eof (drain started),
  /// k::exit_requested (interrupted) or a decoder error.
  /// Not noexcept: note_hole() below it allocates.
  [[nodiscard]] int feed_one(const CancelToken& token) {
    if (pkt_pending_) {
      if ((pkt_->flags & AV_PKT_FLAG_KEY) != 0) awaiting_key_ = false;
      prepare_send();
      const int s = send_held_packet();
      if (s == k::eagain) {
        // Both receive and send report EAGAIN: the decoder violates its contract.
        av_packet_unref(pkt_.get());
        pkt_pending_ = false;
        return k::einval;
      }
      if (s == 0) return 0;
      if (s != k::invalid_data) return s;
      ++total_decode_errors_;
      tainted_ = true;
    }
    if (drained_)
      return k::eof;  // the decoder was drained for a keyframe-only decode; a seek resets it
    for (;;) {
      const int r = read_video_packet(token);
      if (r == k::exit_requested) return r;
      if (r == k::eof) {
        draining_ = true;
        (void)avcodec_send_packet(codec_.get(), nullptr);
        return k::eof;
      }
      const bool key_packet = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
      if (awaiting_key_ && key_flags_reliable_ && !key_packet) {
        // Mid-GOP after a seek: the decoder would decode these in full and drop them anyway.
        av_packet_unref(pkt_.get());
        continue;
      }
      if (key_packet) awaiting_key_ = false;
      prepare_send();
      const int s = send_held_packet();
      if (s == 0 || s == k::eagain) return 0;
      if (s == k::invalid_data) {
        ++total_decode_errors_;
        tainted_ = true;
        if (++decode_errors_ > max_consecutive_errors) return s;
        continue;
      }
      return s;
    }
  }

  /// Before a packet goes to the decoder: non-reference frames far before the target are skipped
  /// at the decoder level (AVDISCARD_NONREF).
  ///
  /// A frame may be skipped only when its display interval provably ends before the window.
  /// Durations cannot prove that (mov stores decode-order deltas, so a VFR stream's frame may say
  /// "1/30 s"), nor can the average frame rate. What does: a frame with a *later* presentation
  /// time has already been fed (B-frames follow their forward reference in decode order) and that
  /// later time is itself before the window.
  void prepare_send() {
    const bool key = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
    AVDiscard skip = AVDISCARD_DEFAULT;
    if (skip_before_ts_ != k::no_pts && key_flags_reliable_ && !key && pkt_->pts != k::no_pts &&
        last_fed_pts_ != k::no_pts) {
      const std::int64_t pts = pkt_->pts;
      if (pts < last_fed_pts_ && last_fed_pts_ <= skip_before_ts_) {
        skip = AVDISCARD_NONREF;
        note_hole(pts);  // if the decoder drops it, nothing may decode across it
      }
    }
    if (codec_->skip_frame != skip) codec_->skip_frame = skip;
  }

  /// A hardware decoder error that is not "bad data" (driver/session failure) triggers the
  /// software fallback; corrupt input is handled like any other corruption. A hardware decoder
  /// that fails before producing its first frame is always treated as unusable.
  [[nodiscard]] bool hardware_fault(int av_err) const noexcept {
    return hw_active_ && (hw_frames_seen_ == 0 || av_err != k::invalid_data);
  }

  /// Core selection loop. Precondition: positioned (or continuing forward).
  [[nodiscard]] std::expected<Selected, Error> select(std::int64_t P, std::int64_t lo,
                                                      std::int64_t hi, const CancelToken& token) {
    for (;;) {
      if (token.requested()) return fail(ErrorCode::cancelled, "cancelled");
      const int r = receive_one();
      if (r == 0) {
        decode_errors_ = 0;
        if (!first_frame_at_) first_frame_at_ = std::chrono::steady_clock::now();
        if (hw_active_) ++hw_frames_seen_;
        if (stream_.synthesize_timestamps) {
          const std::int64_t stamped = synth_ts_ != k::no_pts ? synth_ts_ : stream_.start_pts;
          recv_->pts = recv_->best_effort_timestamp = stamped;
          recv_->duration = stream_.frame_duration_hint;
        }
        const std::int64_t t = frame_ts(*recv_);
        synth_ts_ = t + frame_duration(*recv_);
        last_received_ts_ = t;
        ++received_since_seek_;
        const bool key = (recv_->flags & AV_FRAME_FLAG_KEY) != 0;
        // GOP length estimate (a lower bound, exact at the next keyframe): feeds the back-off step
        // and the forward-scan limit.
        if (last_key_ts_ != k::no_pts && t > last_key_ts_)
          gop_hint_ = std::max(gop_hint_, t - last_key_ts_);
        if (key) last_key_ts_ = t;
        if ((recv_->flags & AV_FRAME_FLAG_CORRUPT) != 0) {
          note_hole(t);  // not produced: nothing may decode across it
          av_frame_unref(corrupt_last_.get());
          av_frame_move_ref(corrupt_last_.get(), recv_.get());
          corrupt_valid_ = true;
          continue;
        }
        // Corrupt: the decoder reported concealment, or a decode error occurred since the last
        // keyframe (the reference chain is suspect).
        if (key) tainted_ = false;
        const bool concealed = recv_->decode_error_flags != 0 || tainted_;
        last_end_ = std::max(last_end_, t + frame_duration(*recv_));
        clear_hole(t);  // whatever was skipped here has now been produced
        if (keyframe_only_) {
          // "The keyframe at or before P": the fed keyframe after a packet-level landing
          // (landing_known_), or the first keyframe out after a seek aimed at P itself. After a
          // back-off (seek_target_ < P) that guarantee is gone: keep decoding, remembering the last
          // keyframe <= P, until a frame past P shows up.
          if (t <= P) {
            if (key || !held_valid_)
              hold_received(concealed);  // a non-key frame only as a fallback
            if (key &&
                (landing_known_ || t == P || (received_since_seek_ == 1 && seek_target_ == P))) {
              return Selected{held_.get(), Adjustment::none, held_concealed_};
            }
            continue;
          }
          if (held_valid_) {
            pend_received(concealed);
            return Selected{held_.get(), Adjustment::none, held_concealed_};
          }
        } else {
          if (t < P) {
            hold_received(concealed);
            if (t >= lo)
              return Selected{held_.get(), Adjustment::none,
                              held_concealed_};  // early accept within tolerance
            continue;
          }
          if (t == P) {
            hold_received(concealed);
            return Selected{held_.get(), Adjustment::none, held_concealed_};
          }
          if (held_valid_) {
            pend_received(concealed);
            return Selected{held_.get(), Adjustment::none, held_concealed_};
          }
        }
        // Overshoot: the first frame out is already past P. Either P precedes the first frame of
        // the stream (provable only after the explicit start seek) or the seek landed late. A late
        // landing is backed off even when a later frame would satisfy the `after` tolerance: the
        // frame actually on screen at P exists until proven otherwise.
        const bool at_start = landed_at_start_ || !stream_.seekable ||
                              (first_frame_ts_ != k::no_pts && t <= first_frame_ts_);
        const bool landing = received_since_seek_ == 1;
        const std::int64_t observed_key = key ? t : k::no_pts;
        if (landing && !at_start) {
          if (auto s = back_off(P, observed_key); !s) return std::unexpected(std::move(s.error()));
          continue;
        }
        if (t <= hi) {
          hold_received(concealed);
          return Selected{held_.get(), Adjustment::none, held_concealed_};
        }
        if (at_start) {
          hold_received(concealed);
          return Selected{held_.get(), Adjustment::clamped_to_first, held_concealed_};
        }
        if (auto s = back_off(P, observed_key); !s) return std::unexpected(std::move(s.error()));
        continue;
      }
      if (r == k::eof) {
        // Nothing decodable between the landing and the end of the stream. Back off unless the
        // start seek has already been done, in which case the stream really has no frame for P.
        if (!held_valid_ && !corrupt_valid_ && stream_.seekable && positioned_ &&
            !landed_at_start_ && !drained_) {
          if (auto s = back_off(P, k::no_pts); !s) return std::unexpected(std::move(s.error()));
          continue;
        }
        // Not one frame came out of the decoder since this request positioned. On a seekable
        // source that says nothing about the stream: the start seek landed and the very first read
        // reported the end, which is what libavio's sticky eof_reached does after a cancellation
        // (a seek does not clear it). Clear it and re-position once before concluding the stream
        // ended — `landed_at_start_` above would otherwise accept that first read as proof.
        if (!held_valid_ && !corrupt_valid_ && stream_.seekable && !drained_ &&
            received_since_seek_ == 0 && !eof_retried_) {
          eof_retried_ = true;
          if (auto s = retry_after_empty_eof(P, token); !s)
            return std::unexpected(std::move(s.error()));
          continue;
        }
        eof_ = true;
        if (last_end_ != std::numeric_limits<std::int64_t>::min()) {
          tail_end_ = tail_end_ == k::no_pts ? last_end_ : std::max(tail_end_, last_end_);
        }
        return finish_at_eof(P);
      }
      if (r == k::eagain) {
        const int f = feed_one(token);
        if (f == 0 || f == k::eof) continue;
        if (f == k::exit_requested) {
          if (token.requested()) return fail(ErrorCode::cancelled, "cancelled");
          return fail(ErrorCode::decode_failed, f,
                      "av_read_frame: interrupted without a cancellation");
        }
        if (hardware_fault(f)) {
          hw_fault_ = true;
          return fail(ErrorCode::decode_failed, f, "avcodec_send_packet (hardware)");
        }
        return fail(ErrorCode::decode_failed, f, "avcodec_send_packet");
      }
      ++total_decode_errors_;
      tainted_ = true;
      if (hardware_fault(r)) {
        hw_fault_ = true;
        return fail(ErrorCode::decode_failed, r, "avcodec_receive_frame (hardware)");
      }
      if (++decode_errors_ > max_consecutive_errors) {
        return fail(ErrorCode::decode_failed, r,
                    "avcodec_receive_frame: too many consecutive errors");
      }
    }
  }

  [[nodiscard]] std::expected<Selected, Error> finish_at_eof(std::int64_t P) {
    const auto finish = [&](FramePtr& f, bool corrupt) -> std::expected<Selected, Error> {
      const std::int64_t t = frame_ts(*f);
      // In keyframe mode the stream extends past the held keyframe to the last frame decoded after
      // it.
      const std::int64_t end = std::max(t + frame_duration(*f), keyframe_only_ ? last_end_ : t);
      if (P < end) return Selected{f.get(), Adjustment::none, corrupt};
      if (opt_.out_of_range == OutOfRangePolicy::clamp_to_last_frame)
        return Selected{f.get(), Adjustment::clamped_to_last, corrupt};
      return fail(
          ErrorCode::time_out_of_range,
          "requested time is past the last frame (" +
              to_string(Time::from_timestamp(std::max<std::int64_t>(t - stream_.start_pts, 0),
                                             from_av(stream_.time_base))) +
              ")");
    };
    if (held_valid_) return finish(held_, held_concealed_);
    if (corrupt_valid_) {
      av_frame_unref(held_.get());
      av_frame_move_ref(held_.get(), corrupt_last_.get());
      held_valid_ = true;
      held_concealed_ = true;
      corrupt_valid_ = false;
      // Nearest-keyframe mode asks for "the keyframe at or before P", and a stashed frame at or
      // before P answers that: its display interval is not the question, as it is in exact mode.
      if (keyframe_only_ && P >= frame_ts(*held_))
        return Selected{held_.get(), Adjustment::none, true};
      return finish(held_, true);
    }
    if (total_decode_errors_ > 0) {
      return fail(
          ErrorCode::decode_failed,
          "no decodable frame found (" + std::to_string(total_decode_errors_) + " decode errors)");
    }
    return fail(ErrorCode::end_of_stream,
                "the stream ended before a frame for the requested time was decoded");
  }

  /// Nearest-keyframe mode answers with the keyframe at or before P without decoding past it, so it
  /// cannot tell a long GOP from the tail of a truncated file: a request past the end would come
  /// back as the last keyframe, unflagged, where exact mode reports time_out_of_range. When the
  /// chosen keyframe is more than a GOP behind the request, read ahead (packets only) until one
  /// proves the stream reaches P or the source ends. Remembered in tail_end_: once per source.
  [[nodiscard]] std::expected<Selected, Error> verify_keyframe_tail(Selected sel, std::int64_t P,
                                                                    const CancelToken& token) {
    if (sel.frame == nullptr) return sel;
    const std::int64_t t = frame_ts(*sel.frame);
    const std::int64_t gop = std::max({gop_hint_, frames_ticks(2), std::int64_t{1}});
    if (P <= t + gop || P <= verified_to_) return sel;  // plainly inside the data
    if (tail_end_ == k::no_pts) {
      // The demuxer sits just after the chosen keyframe: read on until the question is answered.
      for (;;) {
        const int r = read_video_packet(token);
        if (r == k::exit_requested) {
          if (token.requested()) return fail(ErrorCode::cancelled, "cancelled");
          return sel;  // an I/O hiccup is not proof of anything: keep the keyframe
        }
        if (r == k::eof) {
          tail_end_ = verified_to_ != k::no_pts
                          ? verified_to_ + std::max<std::int64_t>(stream_.frame_duration_hint, 1)
                          : t + std::max<std::int64_t>(stream_.frame_duration_hint, 1);
          break;
        }
        av_packet_unref(pkt_.get());
        if (verified_to_ != k::no_pts && verified_to_ > P) break;
      }
      positioned_ = false;  // the demuxer has moved; the next request repositions
    }
    if (tail_end_ == k::no_pts || P < tail_end_) return sel;
    if (opt_.out_of_range == OutOfRangePolicy::clamp_to_last_frame) {
      sel.adjustment = Adjustment::clamped_to_last;
      return sel;
    }
    return fail(
        ErrorCode::time_out_of_range,
        "requested time is past the last frame (" +
            to_string(Time::from_timestamp(std::max<std::int64_t>(tail_end_ - stream_.start_pts, 0),
                                           from_av(stream_.time_base))) +
            ")");
  }

  /// Whether the held frame really is the last frame in presentation order of everything decoded
  /// since positioning. It is, for every monotonic stream. It is not on a container whose
  /// timestamps jump backwards (two recordings concatenated into one MPEG-TS, a camera restart):
  /// decoding forward across the jump ends with a frame from the *earlier* segment, and concluding
  /// "the stream has no frame past this one" from it strands the generator, because the
  /// end-of-stream shortcut then answers every later request without ever repositioning.
  [[nodiscard]] bool held_is_tail() const noexcept {
    if (!held_valid_) return false;
    return frame_ts(*held_) + frame_duration(*held_) >= last_end_;
  }

  /// Positions and selects (the caller converts).
  [[nodiscard]] std::expected<Selected, Error> extract(std::int64_t P, std::int64_t lo,
                                                       std::int64_t hi,
                                                       Adjustment clamped_by_bounds,
                                                       const CancelToken& token) {
    const bool infinite_before = lo == std::numeric_limits<std::int64_t>::min();
    // Nearest-keyframe mode (infinite `before`). Without a usable seek every frame is decoded
    // anyway, so fall back to exact selection instead of returning an arbitrary non-keyframe.
    const bool keyframe_mode = infinite_before && stream_.seekable;
    if (infinite_before && !stream_.seekable) {
      lo = P;
      hi = P;
    }
    // Fast path: the frame covering P is already held.
    if (held_valid_) {
      const std::int64_t h = frame_ts(*held_);
      // A skipped frame between the held frame and P is the one on screen: the held frame does not
      // cover P after all, whatever the look-ahead or the end of stream say.
      if (h <= P && !hole_in(h, P)) {
        if (keyframe_mode) {
          // The held frame is the keyframe covering P (the index says no keyframe lies in (h, P]).
          if ((held_->flags & AV_FRAME_FLAG_KEY) != 0 && key_covers(h, P)) {
            return verify_keyframe_tail(Selected{held_.get(), clamped_by_bounds, held_concealed_},
                                        P, token);
          }
        } else {
          if (pending_valid_ && P < frame_ts(*pending_)) {
            return Selected{held_.get(), clamped_by_bounds, held_concealed_};
          }
          if (eof_ && !pending_valid_ && held_is_tail()) {
            auto s = finish_at_eof(P);
            if (!s) return std::unexpected(std::move(s.error()));
            if (s->adjustment == Adjustment::none) s->adjustment = clamped_by_bounds;
            return *s;
          }
        }
      }
    }
    keyframe_only_ = keyframe_mode;
    // Frames well before the tolerance window may skip their non-reference members. Off in
    // keyframe mode and on inputs without keyframe flags.
    skip_before_ts_ = k::no_pts;
    if (!keyframe_mode && key_flags_reliable_) {
      const std::int64_t margin = reorder_ticks_ + frames_ticks(2);
      skip_before_ts_ = lo > std::numeric_limits<std::int64_t>::min() + margin ? lo - margin : lo;
    }
    if (keyframe_mode) {
      if (auto r = position_for(P, true, token); !r) return std::unexpected(std::move(r.error()));
      const bool pre_edit =
          pkt_pending_ &&
          ((pkt_->flags & AV_PKT_FLAG_DISCARD) != 0 ||
           (first_frame_ts_ != k::no_pts && pkt_->pts != k::no_pts && pkt_->pts < first_frame_ts_));
      if (pre_edit) {
        // The keyframe covering P precedes the first presented frame (an edit list trimmed its
        // GOP). Answer with the first presented frame, clamped, decoded exactly: the decoder drops
        // the trimmed frames itself (AV_PKT_FLAG_DISCARD).
        keyframe_only_ = false;
        P = lo = hi = first_frame_ts_ != k::no_pts ? first_frame_ts_ : stream_.start_pts;
        clamped_by_bounds = Adjustment::keyframe_before_edit;
      } else if (auto r = arm_keyframe_decode(token); !r) {
        return std::unexpected(std::move(r.error()));
      }
    } else if (pending_valid_ && !drained_ && P >= frame_ts(*pending_) &&
               !hole_in(frame_ts(*pending_), P) && forward_is_cheaper(P)) {
      av_frame_unref(held_.get());
      av_frame_move_ref(held_.get(), pending_.get());
      held_valid_ = true;
      held_concealed_ = pending_concealed_;
      pending_valid_ = false;
      backoffs_ = 0;
      backoff_step_ = 0;
    } else if (!(held_valid_ && !pending_valid_ && !eof_ && !drained_ && frame_ts(*held_) <= P &&
                 !hole_in(frame_ts(*held_), P) && forward_is_cheaper(P))) {
      if (auto r = position_for(P, false, token); !r) return std::unexpected(std::move(r.error()));
    } else {
      backoffs_ = 0;
      backoff_step_ = 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int frames_before = cur_.frames_decoded;
    const bool seeked = cur_.seeks > 0;
    first_frame_at_.reset();
    auto sel = select(P, lo, hi, token);
    if (!sel) {
      // A request abandoned before reaching its target fed packets under *its* skip window, and
      // only it knew where that window was: the frames between the decoder's frontier and the
      // window may never be produced, so nothing may continue forward across it. The next request
      // repositions — one seek per cancellation. A source that cannot be repositioned (a pipe)
      // relies on the recorded holes instead, and a request across one fails rather than lies.
      if (skip_before_ts_ != k::no_pts && stream_.seekable) positioned_ = false;
      return std::unexpected(std::move(sel.error()));
    }
    learn_costs(t0, frames_before, seeked);
    if (sel->adjustment == Adjustment::none) sel->adjustment = clamped_by_bounds;
    if (keyframe_mode) return verify_keyframe_tail(*sel, P, token);
    return *sel;
  }

  // Holes: presentation times of frames never produced (AVDISCARD_NONREF, or flagged corrupt).
  // A hole between the decoder's frontier and the request means the frame on screen was never
  // decoded, so nothing may be concluded across one.
  static constexpr std::size_t max_holes = 1u << 14;

  /// Records a frame that may never be produced. The held/look-ahead frames and the
  /// forward-continuation decision consult it, and a request abandoned before its target can leave
  /// a gap ahead of the decoder's frontier (see extract()).
  void note_hole(std::int64_t pts) {
    if (pts == k::no_pts) return;
    const auto it = std::lower_bound(holes_.begin(), holes_.end(), pts);
    if (it != holes_.end() && *it == pts) return;
    if (holes_.size() >= max_holes) {
      holes_.clear();  // pathological: forget everything rather than grow without bound
      return;
    }
    holes_.insert(it, pts);
  }
  /// The frame was decoded after all: it is no longer a gap.
  void clear_hole(std::int64_t pts) noexcept {
    if (holes_.empty() || pts == k::no_pts) return;
    const auto it = std::lower_bound(holes_.begin(), holes_.end(), pts);
    if (it != holes_.end() && *it == pts) holes_.erase(it);
  }
  /// True when a skipped/undecoded frame lies in (after, up_to].
  [[nodiscard]] bool hole_in(std::int64_t after, std::int64_t up_to) const noexcept {
    if (holes_.empty() || up_to <= after) return false;
    const auto it = std::upper_bound(holes_.begin(), holes_.end(), after);
    return it != holes_.end() && *it <= up_to;
  }
  /// Exponential averages of what a seek (positioning + the first frame out of a flushed decoder)
  /// and one further decoded frame cost. A seek request is split at `first_frame_at_`: the part
  /// before it is the seek sample, the frames after it are frame samples.
  void learn_costs(std::chrono::steady_clock::time_point t0, int frames_before,
                   bool seeked) noexcept {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - t0).count();
    const int frames = cur_.frames_decoded - frames_before;
    const auto ema = [](double& acc, double sample) {
      acc = acc > 0 ? acc * 0.7 + sample * 0.3 : sample;
    };
    if (frames <= 0) return;
    if (!seeked) {
      ema(frame_cost_ms_, ms / frames);
      return;
    }
    if (first_frame_at_ && *first_frame_at_ >= t0) {
      ema(seek_cost_ms_, std::chrono::duration<double, std::milli>(*first_frame_at_ - t0).count());
      if (frames > 1)
        ema(frame_cost_ms_,
            std::chrono::duration<double, std::milli>(now - *first_frame_at_).count() /
                (frames - 1));
    } else if (frame_cost_ms_ > 0) {
      ema(seek_cost_ms_, std::max(0.0, ms - (frames - 1) * frame_cost_ms_));
    } else {
      ema(seek_cost_ms_, ms);
    }
  }

  [[nodiscard]] std::expected<Image, Error> convert(Selected sel) {
    const AVFrame* src = sel.frame;
    FramePtr sw;
    if (src->hw_frames_ctx != nullptr) {
      auto t = converter_->download(*src);
      if (!t) {
        hw_fault_ = true;
        return std::unexpected(std::move(t.error()));
      }
      sw = std::move(*t);
      src = sw.get();
    }
    auto out = converter_->convert(*src, stream_.container_sar, stream_.codec_sar, req_max_);
    if (!out) return std::unexpected(std::move(out.error()));
    // A frame before the time origin (MPEG-TS whose start comes from another stream) is reported at
    // zero.
    const std::int64_t t = std::max<std::int64_t>(frame_ts(*sel.frame) - stream_.start_pts, 0);
    const Time actual = Time::from_timestamp(t, from_av(stream_.time_base));
    const bool key = (sel.frame->flags & AV_FRAME_FLAG_KEY) != 0;
    const ColorRange range = from_av(static_cast<AVColorRange>((*out)->color_range));
    const std::int64_t dur = frame_duration(*sel.frame);
    (*out)->duration = dur;
    Image img = ImageAccess::make(std::move(*out), opt_.pixel_format, range,
                                  actual.is_valid() ? actual : Time::zero(), key, sel.adjustment,
                                  sel.corrupt);
    if (dur > 0)
      ImageAccess::set_duration(img, Time::from_timestamp(dur, from_av(stream_.time_base)));
    return img;
  }

  std::string source_;
  Options opt_;

  FormatCtxPtr fmt_;
  AVStream* st_{nullptr};
  const AVCodec* codec_desc_{nullptr};
  CodecCtxPtr codec_;
  BufferRefPtr hw_device_;
  AVHWDeviceType hw_type_{AV_HWDEVICE_TYPE_NONE};
  std::unique_ptr<HwState> hw_state_;
  bool hw_active_{false};
  int hw_frames_seen_{0};
  bool hw_fault_{false};
  bool hw_retried_{false};  ///< the hardware decoder was rebuilt for the current run of failures

  StreamInfo stream_;
  AssetInfo info_;
  mutable std::mutex active_mutex_;
  ActiveDecoder active_;  // guarded by active_mutex_
  std::optional<Converter> converter_;

  PacketPtr pkt_;
  bool pkt_pending_{false};  ///< pkt_ holds a packet the decoder refused with EAGAIN
  FramePtr recv_, held_, pending_, corrupt_last_;
  bool held_valid_{false}, pending_valid_{false}, corrupt_valid_{false};
  bool held_concealed_{false}, pending_concealed_{false};
  bool tainted_{false};  ///< a decode error occurred since the last keyframe
  const AVFrame* probe_frame_{nullptr};
  bool positioned_{false};
  bool landed_at_start_{false};  ///< the last positioning was the explicit start seek or a re-open
  bool awaiting_key_{false};     ///< no keyframe packet fed since the last positioning
  bool first_packet_seen_{false};
  bool key_flags_reliable_{
      false};  ///< the demuxer flags keyframe packets (first packet was flagged)
  bool draining_{false};
  bool eof_{false};
  std::int64_t seek_target_{0};
  std::int64_t last_received_ts_{k::no_pts};
  std::int64_t last_key_ts_{k::no_pts};
  int received_since_seek_{0};  ///< frames out of the decoder since the last positioning
  /// One re-position per request after an end of stream that decoded nothing (see select()).
  bool eof_retried_{false};
  /// Set by interrupt_callback() when it aborts libav I/O, consumed by recover_after_interrupt().
  bool interrupt_fired_{false};
  std::int64_t last_end_{
      std::numeric_limits<std::int64_t>::min()};  ///< furthest frame end seen since positioning
  bool keyframe_only_{false};  ///< the current request is in nearest-keyframe mode
  std::int64_t gop_hint_{0};   ///< largest keyframe spacing observed, in stream ticks
  std::int64_t synth_ts_{k::no_pts};
  std::int64_t first_frame_ts_{k::no_pts};
  std::int64_t backoff_step_{0};
  int backoffs_{0};
  bool drained_{false};     ///< the decoder was drained for a keyframe-only decode; must be flushed
                            ///< (seek) before more input
  bool landing_known_{false};             ///< keyframe mode: the fed keyframe packet is the answer
  std::int64_t last_fed_dts_{k::no_pts};  ///< dts of the last packet sent to the decoder
  std::int64_t last_fed_pts_{k::no_pts};  ///< largest pts sent to the decoder since positioning
  bool keys_have_dts_{false};             ///< keyframe packets carry a dts (mov); Matroska's do not
  std::int64_t reorder_ticks_{
      0};  ///< a keyframe's pts - dts (the B-frame reorder delay), smallest seen
  bool reorder_known_{false};
  std::int64_t seek_bias_{0};  ///< subtracted from seek targets on demuxers that search a DTS index
                               ///< with an unshifted PTS (fragmented MP4)
  double seek_cost_ms_{0};      ///< learned: positioning + first frame after a flush
  double frame_cost_ms_{0};     ///< learned: one further decoded frame
  std::optional<std::chrono::steady_clock::time_point>
      first_frame_at_;  ///< when the current request's first frame came out
  std::vector<KeyEntry>
      key_index_;  ///< keyframe packets seen (containers without a trusted index), by pts
  std::size_t contig_key_{
      no_entry};  ///< entry of the last keyframe read without a seek since (its GOP extent grows)
  PacketPtr land_pkt_;  ///< keyframe mode: the chosen keyframe packet while scanning past it
  std::optional<Size> req_max_;  ///< RequestOptions::maximum_size of the current request
  std::int64_t skip_before_ts_{
      k::no_pts};  ///< packets whose frames end before this may skip non-reference frames
  std::int64_t verified_to_{
      k::no_pts};  ///< largest packet presentation time actually read from the source
  std::int64_t tail_end_{k::no_pts};  ///< end of the data once the end of stream has been observed
                                      ///< (k::no_pts = not yet)
  std::vector<std::int64_t> holes_;  ///< presentation times of frames skipped (NONREF) or dropped
                                     ///< (corrupt) since the last reposition, sorted
  std::vector<PacketPtr> gop_buffer_;  ///< exact mode: the chosen keyframe's packets scanned past
                                       ///< P, replayed to the decoder
  std::size_t gop_buffer_bytes_{0};
  std::size_t replay_pos_{0};
  bool replaying_{false};  ///< the scan is over; read_video_packet serves gop_buffer_ first
  int demux_errors_{0};
  int decode_errors_{0};
  int seek_failures_{0};  ///< consecutive avformat_seek_file failures (interrupts excluded)
  long total_decode_errors_{0};
  const CancelToken* current_token_{nullptr};
  std::string
      broken_reason_;  ///< why the last re-open failed (the pipeline retries on the next request)

  /// What the current request has cost so far, reset at the start of each one. Not observability:
  /// learn_costs() reads frames_decoded, and forward_is_cheaper() decides seek-versus-decode-forward
  /// from the exponential averages learn_costs() maintains. Decoding thread only.
  struct RequestStats {
    int seeks{0};           ///< avformat_seek_file / av_seek_frame calls made for this request
    int frames_decoded{0};  ///< frames received from the decoder, look-ahead and skipped included
  };
  RequestStats cur_;
};

}  // namespace stills::detail

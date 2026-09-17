#pragma once
// stills/detail/pipeline.hpp — the decoder and everything built on it: hardware setup with
// software fallback, accurate seeking, the decode loop, frame selection, and conversion to the
// output Image.
//
// The container and its I/O live in MediaSource (detail/media_source.hpp), which this owns and
// drives. Anything reaching the AVFormatContext goes through it. The packets themselves, the GOP
// replay buffer and the landing scan live in PacketReader (detail/packet_reader.hpp); where the
// keyframes are lives in KeyframeIndex (detail/keyframe_index.hpp). Both are owned here and driven
// here, and neither knows about this class or about each other's owner.
//
// A Pipeline is single-threaded by contract: callers serialise access (see worker.hpp).

#include <algorithm>
#include <cstdint>
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
#include "stills/detail/decode_frontier.hpp"
#include "stills/detail/ffmpeg.hpp"
#include "stills/detail/frame_slot.hpp"
#include "stills/detail/hw.hpp"
#include "stills/detail/keyframe_index.hpp"
#include "stills/detail/media_source.hpp"
#include "stills/detail/packet_reader.hpp"
#include "stills/detail/seek_cost_model.hpp"
#include "stills/image.hpp"
#include "stills/options.hpp"
#include "stills/time.hpp"

namespace stills::detail {

class Pipeline {
 public:
  /// A frame chosen for a request, owned by the pipeline (one of its FrameSlots).
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
    p->source_.setCancelToken(token);
    auto finish = [&](Error e) -> std::expected<std::unique_ptr<Pipeline>, Error> {
      p->source_.setCancelToken(nullptr);
      if (token != nullptr && token->requested())
        return fail(ErrorCode::cancelled, "open cancelled");
      return std::unexpected(std::move(e));
    };
    if (auto r = p->source_.open(p->opt_); !r) return finish(std::move(r.error()));
    if (auto r = p->attach_to_source(); !r) return finish(std::move(r.error()));
    if (auto r = p->setup_decoder(); !r) return finish(std::move(r.error()));
    p->fill_info();
    p->source_.setCancelToken(nullptr);
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

  // Seeks and decoded frames attributable to the request that just returned: cur_ is reset at the
  // top of image_at() and nowhere else. Both counters are maintained for SeekCostModel and
  // isForwardCheaperThanSeek(), so reading them adds no work to the decode path and a build that
  // never calls these pays nothing. The benchmark harness (tests/bench) uses them to check that a
  // restructure changes neither count -- which seeks happen is the behaviour, the milliseconds are
  // only the machine.
  //
  // The seeks are a difference rather than a counter of their own: MediaSource counts every seek it
  // issues, because that is where the calls are made and a count kept anywhere else could drift out
  // of step with them, and a request is not a scope MediaSource knows about.
  [[nodiscard]] int getSeekCount() const noexcept {
    return static_cast<int>(source_.getSeekCallCount() - cur_.seeksAtStart);
  }
  [[nodiscard]] int getDecodedFrameCount() const noexcept { return cur_.frames_decoded; }

  /// Extracts the frame for `requested` (asset-relative). Thread-unsafe by design.
  [[nodiscard]] std::expected<Image, Error> image_at(Time requested, const CancelToken& token) {
    return image_at(requested, RequestOptions{}, token);
  }
  /// The decode path allocates freely and the async worker's loop is noexcept, so a std::bad_alloc
  /// below here would terminate instead of surfacing the out_of_memory the API defines. Caught at
  /// this frame, the only one that can drop the half-updated state: the next request re-positions.
  [[nodiscard]] std::expected<Image, Error> image_at(Time requested, const RequestOptions& ro,
                                                     const CancelToken& token) {
    cur_ = RequestStats{.seeksAtStart = source_.getSeekCallCount()};
    try {
      return image_at_impl(requested, ro, token);
    } catch (const std::bad_alloc&) {
      source_.setCancelToken(nullptr);
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
    source_.setCancelToken(&token);
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
        source_.setCancelToken(nullptr);
        return std::unexpected(make_error(ErrorCode::hardware_unavailable, 0,
                                          "hardware decoding failed: " + result.error().message));
      }
      if (auto r = rebuild_software("hardware decoder failed during decoding: " +
                                    result.error().message);
          !r) {
        source_.setCancelToken(nullptr);
        return std::unexpected(std::move(r.error()));
      }
      result = attempt();
    }
    if (result && hw_active_)
      hw_retried_ = false;  // a successful hardware request re-arms the retry
    source_.setCancelToken(nullptr);
    // AVERROR_EXIT with a cancelled token is the cancellation; with a live token it is a stale
    // interrupt and stays an I/O failure.
    if (!result && result.error().av_error == k::exit_requested && token.requested()) {
      return fail(ErrorCode::cancelled, "cancelled");
    }
    return result;
  }

  /// Positions and selects the frame for `requested` without converting it (the frame stays owned
  /// by the pipeline, in one of its FrameSlots). Validates the request, maps the time into stream
  /// ticks, applies the bounds policy and the tolerance window.
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
        requested.to_timestamp(from_av(stream().time_base), TimeRounding::nearest);
    if (rel == std::numeric_limits<std::int64_t>::max()) {
      return fail(ErrorCode::time_out_of_range, "requested time does not fit the stream time base");
    }
    std::int64_t target = 0;
    if (__builtin_add_overflow(stream().start_pts, rel, &target)) {
      // A representable Time can still fall outside the stream's timestamp domain once the origin
      // is added (an MPEG-TS starting at 10 s). Report it rather than wrapping.
      return fail(ErrorCode::time_out_of_range, "requested time does not fit the stream time base");
    }
    Adjustment clamped = Adjustment::none;
    // Containers occasionally understate their duration by a frame; give one frame of slack before
    // rejecting up front, and let the decoder (EOF) decide inside that margin.
    if (stream().duration_pts &&
        rel > *stream().duration_pts + std::max<std::int64_t>(stream().frame_duration_hint, 0)) {
      if (opt_.out_of_range == OutOfRangePolicy::error) {
        return fail(
            ErrorCode::time_out_of_range,
            to_string(requested) + " is beyond the asset duration " +
                to_string(Time::from_timestamp(*stream().duration_pts,
                                               from_av(stream().time_base))));
      }
      target = stream().start_pts + *stream().duration_pts;
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
        ticks = tol.to_timestamp(from_av(stream().time_base), TimeRounding::down);
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

    if (!source_.isOpen() || !codec_) {
      // A previous re-open failed (or was cancelled): retry rather than staying dead forever.
      if (auto r = reopen(); !r) {
        return fail(ErrorCode::unusable, r.error().av_error,
                    "the generator is unusable: " + broken_reason_);
      }
    }
    if (!recover_after_interrupt()) {
      // The interrupt left libavio's state stuck and this libavformat major is not one
      // MediaSource::tryResetIoState() can clear: re-open instead of reading through it.
      if (auto r = reopen(); !r) return std::unexpected(std::move(r.error()));
    }
    eof_retried_ = false;  // one re-position per attempt (a hardware fallback retries the request)
    return extract(target, lo, hi, clamped, token);
  }

  static constexpr int max_consecutive_errors = 32;
  /// Hard cap on landing back-offs per request; the doubling step reaches the start long before.
  static constexpr int max_backoffs = 64;

  Pipeline(std::string source, Options options)
      : source_(std::move(source)), opt_(std::move(options)) {}

  // What the source learned about the chosen video stream. MediaSource is the only writer; the
  // handful of fields the pipeline has to change (the time origin, the decoder's frame rate,
  // seekability after a run of failed seeks) go back through named MediaSource methods, so no
  // caller can set one behind its back.
  [[nodiscard]] const StreamInfo& stream() const noexcept { return source_.getStreamInfo(); }

 public:
  ~Pipeline() = default;

 private:
  /// The pipeline's own half of opening: the decoder libavformat picked for the stream, the packet
  /// and frame buffers, and the converter (whose orientation comes from the stream's display
  /// matrix). Runs after every MediaSource open and re-open, and owns nothing MediaSource owns.
  [[nodiscard]] std::expected<void, Error> attach_to_source() {
    codec_desc_ = source_.getCodec();
    if (auto r = packets_.attach(); !r) return r;
    {
      auto fr2 = make_frame();
      if (!fr2) return std::unexpected(fr2.error());
      recv_ = std::move(*fr2);
    }
    for (FrameSlot* slot : {&held_, &pending_, &corrupt_last_}) {
      auto fr2 = make_frame();
      if (!fr2) return std::unexpected(fr2.error());
      slot->install(std::move(*fr2));
    }
    const DisplayTransform applied =
        opt_.apply_preferred_track_transform ? stream().transform : DisplayTransform{};
    converter_.emplace(opt_.pixel_format, opt_.scaler, opt_.maximum_size,
                       opt_.apply_sample_aspect_ratio, applied.rotation, applied.mirrored,
                       /*strip_display_matrix=*/opt_.apply_preferred_track_transform);
    return {};
  }

  [[nodiscard]] std::expected<void, Error> build_codec(const HwCandidate* hw) {
    hw_fault_ = false;  // belongs to the decoder being replaced
    codec_.reset();
    hw_device_.reset();
    hw_state_.reset();
    reset_position();

    CodecCtxPtr cc{avcodec_alloc_context3(codec_desc_)};
    if (!cc) return fail(ErrorCode::out_of_memory, "avcodec_alloc_context3");
    if (int r = avcodec_parameters_to_context(cc.get(), source_.getStream()->codecpar); r < 0) {
      return fail(ErrorCode::decoder_open_failed, r, "avcodec_parameters_to_context");
    }
    // required for best_effort_timestamp / frame->duration
    cc->pkt_timebase = source_.getStream()->time_base;
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
    frontier.skipped.clear();
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
    if (stream().synthesize_timestamps && cc->framerate.num > 0 && cc->framerate.den > 0) {
      source_.adoptDecoderFrameRate(cc->framerate);
    }
    if (stream().synthesize_timestamps && stream().frame_duration_hint <= 0) {
      source_.adoptDecoderFrameRate(AVRational{25, 1});  // libav's own default
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
  [[nodiscard]] static bool isHardwareWorthwhile(const AVCodecParameters& par,
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
      effective.hardware.policy = isHardwareWorthwhile(*source_.getStream()->codecpar, *codec_desc_)
                                      ? HardwarePolicy::prefer_hardware
                                      : HardwarePolicy::software_only;
    }
    std::vector<HwCandidate> candidates = hw_candidates(codec_desc_, effective, reason);
    if (opt_.hardware.policy == HardwarePolicy::automatic &&
        effective.hardware.policy == HardwarePolicy::software_only) {
      const AVCodecParameters& par = *source_.getStream()->codecpar;
      reason = "automatic policy: software decoding is faster for " + std::to_string(par.width) +
               "x" + std::to_string(par.height) + " " + codec_desc_->name;
    }
    if (!candidates.empty() && !stream().io_seekable) {
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
      seek_target_ = stream().start_pts;
    }
    auto sel = select(stream().start_pts, std::numeric_limits<std::int64_t>::min(),
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
    // Raw streams: anchor Time::zero() at the first frame.
    if (source_.needsStartPtsFromFirstFrame()) source_.setStartPts(first_frame_ts_);
    return {};
  }

  void set_active(ActiveDecoder a) {
    a.decoder_threads = codec_ ? codec_->thread_count : 0;
    std::lock_guard lk(active_mutex_);
    active_ = std::move(a);
  }

  [[nodiscard]] AVRational effective_sar() const noexcept {
    if (stream().container_sar.num > 0 && stream().container_sar.den > 0)
      return stream().container_sar;
    if (probe_frame_ != nullptr && probe_frame_->sample_aspect_ratio.num > 0 &&
        probe_frame_->sample_aspect_ratio.den > 0) {
      return probe_frame_->sample_aspect_ratio;
    }
    if (stream().codec_sar.num > 0 && stream().codec_sar.den > 0) return stream().codec_sar;
    return AVRational{1, 1};
  }

  void fill_info() {
    info_ = AssetInfo{};
    info_.container_name = source_.getContainerName();
    info_.codec_name = codec_desc_->name;
    const auto src_fmt = static_cast<AVPixelFormat>(source_.getStream()->codecpar->format);
    const char* fmt_name = av_get_pix_fmt_name(src_fmt);
    if (fmt_name == nullptr && probe_frame_ != nullptr) {
      fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(probe_frame_->format));
    }
    info_.source_pixel_format = fmt_name != nullptr ? fmt_name : "";
    info_.video_stream_index = stream().index;
    info_.time_base = from_av(stream().time_base);
    info_.average_frame_rate =
        stream().avg_frame_rate.num > 0 ? from_av(stream().avg_frame_rate) : Rational{0, 1};
    if (stream().duration_pts) {
      info_.duration = Time::from_timestamp(*stream().duration_pts, info_.time_base);
    }
    if (source_.getStream()->nb_frames > 0) info_.frame_count = source_.getStream()->nb_frames;
    Size coded{source_.getStream()->codecpar->width, source_.getStream()->codecpar->height};
    if (probe_frame_ != nullptr && probe_frame_->width > 0)
      coded = Size{probe_frame_->width, probe_frame_->height};
    info_.coded_size = coded;
    const AVRational sar = effective_sar();
    const int rot = opt_.apply_preferred_track_transform ? stream().transform.rotation : 0;
    info_.display_size = display_size(coded, sar, opt_.apply_sample_aspect_ratio, rot);
    info_.output_size = converter_->output_size(coded, sar);
    info_.rotation_degrees = stream().transform.rotation;
    info_.mirrored = stream().transform.mirrored;
    info_.sample_aspect_ratio = from_av(sar);
    info_.seekable = stream().seekable;
    info_.timestamps_synthesized = stream().synthesize_timestamps;
    const auto name_or_empty = [](const char* n) {
      return n != nullptr ? std::string{n} : std::string{};
    };
    const AVCodecParameters& par = *source_.getStream()->codecpar;
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

  void reset_position() noexcept {
    av_frame_unref(recv_.get());
    held_.clear();
    pending_.clear();
    corrupt_last_.clear();
    frontier.reset();  // including the recorded holes: nothing before a reposition can matter
    landing_known_ = false;
    decode_errors_ = 0;
    probe_frame_ = nullptr;
    keys_.resetContiguity();
    packets_.resetPosition();
  }

  void after_seek(std::int64_t target, bool at_start) noexcept {
    avcodec_flush_buffers(codec_.get());
    reset_position();
    awaiting_key_ = true;
    positioned_ = true;
    landed_at_start_ = at_start;
    seek_target_ = target;
    frontier.synthTs = target;
  }

  /// Seeks towards `target`. Where the seek actually landed is established by read_landing().
  [[nodiscard]] std::expected<void, Error> seek_to(std::int64_t target) {
    const int r = source_.seekTo(target);
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
    const int r = source_.byteSeekTo(e.pos);
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
    if (!stream().seekable) {
      if (!stream().io_seekable) {
        return fail(ErrorCode::not_seekable, "the source cannot be rewound");
      }
      return reopen();
    }
    const int r = source_.seekToStart();
    if (r < 0) {
      reset_position();
      positioned_ = false;
      if (r == k::exit_requested)
        return fail(ErrorCode::seek_failed, r, "avformat_seek_file(start)");
      if (!stream().io_seekable) {
        return fail(ErrorCode::not_seekable, r,
                    "seeking to the start failed and the source cannot be re-opened");
      }
      return reopen();
    }
    after_seek(stream().start_pts, true);
    return {};
  }

  /// Re-establishes the source from scratch (non-seekable sources that must rewind, or after a
  /// failed seek) and rebuilds the decoder on top of it.
  ///
  /// MediaSource::reopen() re-opens the container and returns: the container and the decoder have
  /// different lifetimes (this one container outlives two or three decoder rebuilds on the hardware
  /// fallback path), so the order of the two is decided here rather than buried in the re-open.
  /// build_codec() is what resets the decode position, exactly as it does for every other rebuild.
  ///
  /// No re-open under an already-cancelled request: the re-probe runs its I/O through the interrupt
  /// callback and would keep only what it managed to read. Checked here rather than inside
  /// MediaSource, because by the time the container is touched this has already given up its
  /// decoder, and a cancelled request must not pay that.
  [[nodiscard]] std::expected<void, Error> reopen() {
    if (source_.isCancelled()) {
      return fail(ErrorCode::cancelled, k::exit_requested,
                  "cancelled before re-opening the source");
    }
    const bool was_hw = hw_active_ && hw_state_ != nullptr && hw_device_ != nullptr;
    HwCandidate hw{};
    if (was_hw) {
      hw.pix_fmt = hw_state_->hw_pix_fmt;
      hw.type = hw_type_;
    }
    codec_.reset();
    positioned_ = false;
    auto r = source_.reopen(opt_);
    if (r) r = attach_to_source();
    if (r) r = build_codec(was_hw ? &hw : nullptr);
    if (!r) {
      // The old context is gone and the new one failed; image_at() retries on the next request.
      // close() is idempotent: MediaSource::reopen() has already closed when it was the step that
      // failed, and has not when attach_to_source() or build_codec() was.
      codec_.reset();
      source_.close();
      broken_reason_ = r.error().message;
      return fail(ErrorCode::seek_failed, r.error().av_error,
                  "re-opening the source: " + r.error().message);
    }
    tail_end_ = k::no_pts;  // a re-opened (or grown) source may reach further than it did
    packets_.resetVerifiedTo();
    positioned_ = true;
    awaiting_key_ = true;
    landed_at_start_ = true;
    seek_target_ = stream().start_pts;
    return {};
  }

  /// Throws away everything the decoder and this pipeline believed after an interrupt aborted a
  /// libavformat read. MediaSource clears libavio's sticky state and flushes the demuxer; the
  /// decoder flush, the frontier and the position are this pipeline's to drop.
  ///
  /// Returns false when the sticky state could not be cleared in place (see
  /// MediaSource::recoverAfterInterrupt) and the source can be re-opened. The caller must then
  /// re-establish it before the next read; nothing else clears the I/O layer.
  [[nodiscard]] bool recover_after_interrupt() noexcept {
    const MediaSource::IoState io = source_.recoverAfterInterrupt();
    if (io == MediaSource::IoState::clean) return true;
    const std::int64_t last = frontier.lastReceivedTs;
    if (codec_) avcodec_flush_buffers(codec_.get());
    reset_position();
    if (io == MediaSource::IoState::stuck && stream().io_seekable) {
      positioned_ = false;  // nothing here can unstick the I/O layer; the caller re-opens
      return false;
    }
    if (stream().seekable) {
      positioned_ = false;  // forces a seek in position_for()
    } else {
      // Best effort on a non-rewindable input: the demuxer is somewhere at or after the last frame.
      // The one field carried across reset(): the rest of the frontier described a decoder state
      // the flush above destroyed, but this one is still true of the source.
      positioned_ = true;
      landed_at_start_ = false;
      frontier.lastReceivedTs = last;
    }
    return true;
  }

  /// Re-positions for `P` after an end of stream that produced no frames at all. libavio's sticky
  /// end-of-file is cleared first: a seek leaves it set, so the retry would read EOF again. On a
  /// libavformat major where it cannot be cleared in place, the source is re-opened instead.
  [[nodiscard]] std::expected<void, Error> retry_after_empty_eof(std::int64_t P,
                                                                 const CancelToken& token) {
    if (!source_.tryResetIo() && stream().io_seekable) {
      // An unverified libavformat major: the sticky end-of-file survives a seek, so the source is
      // re-established rather than retried in place. Slower by one re-open, and correct.
      reset_position();
      if (auto r = reopen(); !r) return r;
      return position_for(P, keyframe_only_, token);
    }
    source_.flushDemuxer();
    if (codec_) avcodec_flush_buffers(codec_.get());
    reset_position();
    positioned_ = false;  // forces a real seek rather than a decode-forward
    return position_for(P, keyframe_only_, token);
  }

  /// Seeks towards `target`, with the failure policy: interrupted I/O is reported as is, other
  /// failures count towards giving up on seeking and fall back to a re-open.
  [[nodiscard]] std::expected<void, Error> seek_or_reopen(std::int64_t target) {
    if (target <= stream().start_pts) return seek_to_start();
    auto r = seek_to(target);
    if (r) {
      source_.noteSeekSucceeded();
      return {};
    }
    if (r.error().av_error == k::exit_requested) return r;
    source_.noteSeekFailed();
    if (!stream().io_seekable) {
      return fail(ErrorCode::not_seekable, r.error().av_error,
                  "seeking failed and the source cannot be re-opened");
    }
    return reopen();
  }

  [[nodiscard]] std::int64_t one_second() const noexcept {
    return av_rescale_q(1, AVRational{1, 1}, stream().time_base);
  }
  [[nodiscard]] std::int64_t frames_ticks(int n) const noexcept {
    return stream().frame_duration_hint > 0 ? n * stream().frame_duration_hint : 0;
  }

  /// Whether the decoder can still reach `target` by decoding on from where it is.
  ///
  /// The invariant: continuing forward answers with frames the decoder has yet to produce, so it
  /// is sound only while every frame between the decoder's frontier and `target` will actually
  /// come out. A gap there — a non-reference frame skipped for a request that was abandoned before
  /// it got there — would be the frame on screen, and nothing may be concluded across one.
  [[nodiscard]] bool canDecodeForwardTo(std::int64_t target) const noexcept {
    return positioned_ && !frontier.drained && !frontier.eof &&
           frontier.lastReceivedTs != k::no_pts && target >= frontier.lastReceivedTs &&
           !frontier.skipped.containsIn(frontier.lastReceivedTs, target);
  }

  /// Decides between continuing to decode forward and seeking, then positions accordingly. In
  /// keyframe mode the answer is a keyframe packet that is then decoded alone
  /// (arm_keyframe_decode).
  [[nodiscard]] std::expected<void, Error> position_for(std::int64_t P, bool keyframe_mode,
                                                        const CancelToken& token) {
    backoffs_ = 0;
    backoff_step_ = 0;
    if (!stream().seekable) {
      if (canDecodeForwardTo(P)) return {};
      if (positioned_ && !frontier.drained && !frontier.eof &&
          frontier.lastReceivedTs == k::no_pts && landed_at_start_)
        return {};
      if (!stream().io_seekable) {
        return fail(
            ErrorCode::not_seekable,
            "the source is not seekable; requested times must not precede the current position");
      }
      return reopen();
    }
    if (!keyframe_mode) {
      if (canDecodeForwardTo(P) && isForwardCheaperThanSeek(P)) return {};
      if (positioned_ && !frontier.drained && !frontier.eof &&
          frontier.lastReceivedTs == k::no_pts && seek_target_ <= P &&
          P - seek_target_ <= forward_scan_limit()) {
        return {};  // freshly positioned just before the target; the landing is verified in
                    // select()
      }
    }
    if (stream().index_trusted) return position_indexed(P, keyframe_mode, token);
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
      if (!stream().seekable) return {};  // gave up on seeking: the re-open positioned at the start
      auto land = read_landing(P, /*scan=*/false, keyframe_mode, token);
      if (!land) return std::unexpected(std::move(land.error()));
      if (land->found || landed_at_start_ || !packets_.areKeyFlagsReliable()) return {};
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
      // Read after the landing, not before it: the packets it just read may have lowered the delay.
      const std::int64_t reorder_ticks = keys_.getReorderTicks();
      const std::int64_t delay = (kd != k::no_pts && kp > kd) ? kp - kd : reorder_ticks;
      if (kd != k::no_pts && reorder_ticks > 0 && kd > P - reorder_ticks && seek_bias_ == 0) {
        // The demuxer searched its DTS index with our PTS unshifted (fragmented MP4): remember the
        // reorder delay so the next seek aims right. Open-GOP overshoots (the CRA's DTS is already
        // below P - delay) must not set it, or every seek would land a GOP early.
        seek_bias_ = reorder_ticks;
      }
      if (round >= 3 || backoffs_ >= max_backoffs)
        return seek_to_start_and_land(P, keyframe_mode, token);
      target = kp - 1 - delay -
               (round > 0 ? std::max({one_second(), keys_.getGopHint(), std::int64_t{1}}) * round
                          : 0);
      ++backoffs_;
    }
  }

  [[nodiscard]] std::expected<void, Error> seek_to_start_and_land(std::int64_t P,
                                                                  bool keyframe_mode,
                                                                  const CancelToken& token) {
    if (auto r = seek_to_start(); !r) return r;
    auto land = read_landing(P, /*scan=*/!stream().index_trusted, keyframe_mode, token);
    if (!land) return std::unexpected(std::move(land.error()));
    return {};
  }

  /// Containers without a trusted index (MPEG-TS): position with a byte seek when the keyframe
  /// covering P is already known; otherwise aim one GOP early, scan the packets up to the target
  /// and choose the last keyframe at or before it (recording every keyframe on the way).
  [[nodiscard]] std::expected<void, Error> position_scanned(std::int64_t P, bool keyframe_mode,
                                                            const CancelToken& token) {
    if (const KeyEntry* e = keys_.findCoveringKey(P); e != nullptr) {
      if (auto r = byte_seek(*e); !r) return r;
      if (!keyframe_mode) return {};
      auto land = read_landing(P, /*scan=*/false, keyframe_mode, token);
      if (!land) return std::unexpected(std::move(land.error()));
      if (land->found) return {};
      // The recorded position no longer holds (the file changed): fall through to a fresh scan.
    }
    const std::int64_t margin = frames_ticks(2) + keys_.getReorderTicks();
    const std::int64_t gop_hint = keys_.getGopHint();
    std::int64_t aim = P - margin - (gop_hint > 0 ? gop_hint : 0);
    if (auto r = seek_or_reopen(aim); !r) return r;
    for (;;) {
      if (!stream().seekable) return {};
      auto land = read_landing(P, /*scan=*/true, keyframe_mode, token);
      if (!land) return std::unexpected(std::move(land.error()));
      if (land->found || landed_at_start_ || !packets_.areKeyFlagsReliable()) return {};
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
    if (backoff_step_ <= 0)
      backoff_step_ = std::max({one_second(), keys_.getGopHint(), std::int64_t{1}});
    const std::int64_t margin = frames_ticks(2) + keys_.getReorderTicks();
    const std::int64_t anchor =
        std::min(P, observed_key != k::no_pts ? observed_key : seek_target_);
    std::int64_t earlier = anchor - backoff_step_ - margin;
    if (earlier >= seek_target_)
      earlier = seek_target_ - backoff_step_;  // always strictly earlier than last time
    if (backoff_step_ < std::numeric_limits<std::int64_t>::max() / 2) backoff_step_ *= 2;
    if (++backoffs_ >= max_backoffs || earlier <= stream().start_pts) return seek_to_start();
    return seek_or_reopen(earlier);
  }

  /// Without a trusted index, decode forward when the target is within one GOP (at least 3 s):
  /// a seek would land in the same or the next GOP and decode about as many frames anyway.
  [[nodiscard]] std::int64_t forward_scan_limit() const noexcept {
    const std::int64_t three_seconds = av_rescale_q(3, AVRational{1, 1}, stream().time_base);
    return std::max(three_seconds, keys_.getGopHint());
  }

  /// True when decoding forward to `target` is cheaper than seeking: the keyframe at or before the
  /// target has already been fed to the decoder (both sides in the container's own timestamp
  /// domain), or the distance is short. Cost-aware for slow seeks (hardware): frames between the
  /// current position and that keyframe are decoded forward while they cost less than a seek.
  [[nodiscard]] bool isForwardCheaperThanSeek(std::int64_t target) const noexcept {
    std::int64_t key_ts = k::no_pts;  // the covering keyframe, in the same domain as fed_ts
    std::int64_t fed_ts = k::no_pts;
    bool covered = false;
    if (stream().index_trusted) {
      if (const AVIndexEntry* kf = keys_.getIndexKeyBefore(target, containerIndexOf(source_));
          kf != nullptr) {
        key_ts = kf->timestamp;
        fed_ts = keys_.pickIndexTs(frontier.lastFedDts, frontier.lastFedPts);
        covered = true;
      }
    } else if (const KeyEntry* e = keys_.findCoveringKey(target); e != nullptr) {
      key_ts = e->dts != k::no_pts ? e->dts : e->pts;
      fed_ts = e->dts != k::no_pts ? frontier.lastFedDts : frontier.lastFedPts;
      covered = true;
    }
    if (covered && key_ts != k::no_pts && fed_ts != k::no_pts) {
      // The covering keyframe has been fed: the target is downstream of the decoder's state.
      if (key_ts <= fed_ts + std::max<std::int64_t>(stream().frame_duration_hint, 0)) return true;
      // A keyframe lies ahead: forward wins while the frames up to it cost less than a seek (a
      // real trade on hardware decoders and tiny frames).
      if (costs.getFrameCostMs() > 0 && stream().frame_duration_hint > 0) {
        const double frames_to_key = static_cast<double>(key_ts - fed_ts) /
                                     static_cast<double>(stream().frame_duration_hint);
        return frames_to_key * costs.getFrameCostMs() < costs.getSeekCostMs();
      }
      return false;
    }
    if (stream().index_trusted)
      return target - frontier.lastReceivedTs <=
             frames_ticks(
                 4);  // the index does not cover P (unread fragment): seeks are exact there
    return target - frontier.lastReceivedTs <= forward_scan_limit();
  }

  [[nodiscard]] std::int64_t frame_ts(const AVFrame& f) const noexcept {
    if (f.best_effort_timestamp != k::no_pts) return f.best_effort_timestamp;
    if (f.pts != k::no_pts) return f.pts;
    if (f.pkt_dts != k::no_pts) return f.pkt_dts;
    return frontier.synthTs != k::no_pts ? frontier.synthTs : stream().start_pts;
  }

  [[nodiscard]] std::int64_t frame_duration(const AVFrame& f) const noexcept {
    return f.duration > 0 ? f.duration : stream().frame_duration_hint;
  }

  // The received frame becomes the answer. The look-ahead slot is emptied with it: no frame past
  // the new held one has been seen yet, and a stale one there would claim it covers the request.
  void hold_received(bool concealed) noexcept {
    held_.adopt(*recv_, concealed);
    pending_.clear();
  }

  // One frame of look-ahead past the held frame -- what proves the held frame covers the request.
  void pend_received(bool concealed) noexcept { pending_.adopt(*recv_, concealed); }

  /// Pulls one frame. Returns 0 (frame in recv_), k::eof, k::eagain (needs a packet) or an error.
  [[nodiscard]] int receive_one() noexcept {
    av_frame_unref(recv_.get());
    const int r = avcodec_receive_frame(codec_.get(), recv_.get());
    if (r == 0) {
      ++cur_.frames_decoded;
    }
    if (r == k::eagain && frontier.draining) return k::eof;
    return r;
  }

  /// Sends the reader's live packet. Returns 0 (consumed), k::eagain (kept for a retry after the
  /// decoder has been drained) or an error (packet dropped).
  [[nodiscard]] int send_held_packet() noexcept {
    AVPacket& pkt = packets_.getPacket();
    const std::int64_t fed_dts = pkt.dts;
    const std::int64_t fed_pts = pkt.pts;
    const int s = avcodec_send_packet(codec_.get(), &pkt);
    if (s == k::eagain) {
      packets_.holdPacketForRetry();  // the decoder wants a receive first; keep the packet
      return s;
    }
    if (s == 0) {
      if (fed_dts != k::no_pts) frontier.lastFedDts = fed_dts;
      if (fed_pts != k::no_pts)
        frontier.lastFedPts =
            frontier.lastFedPts == k::no_pts ? fed_pts : std::max(frontier.lastFedPts, fed_pts);
    }
    packets_.releasePacket();
    return s;
  }

  /// PacketReader::readLanding() plus the part only positioning may carry out. The scan reads
  /// packets and chooses the keyframe; going back to one is a seek, and seeks are not the
  /// reader's. Returned as data rather than done in place so the dependency runs one way:
  /// positioning drives the reader, never the other way round.
  [[nodiscard]] std::expected<PacketReader::Landing, Error> read_landing(
      std::int64_t P, bool scan, bool keyframe_mode, const CancelToken& token) {
    auto land =
        packets_.readLanding(source_, keys_, P, scan, keyframe_mode, frontier.tainted, token);
    if (!land) return land;
    if (land->awaitKey) awaiting_key_ = true;
    switch (land->rewind) {
      case PacketReader::Landing::Rewind::none:
        break;
      case PacketReader::Landing::Rewind::byteSeek:
        if (auto r = byte_seek(land->rewindEntry); !r) return std::unexpected(std::move(r.error()));
        break;
      case PacketReader::Landing::Rewind::timestampSeek:
        if (auto r = seek_or_reopen(land->rewindTs); !r)
          return std::unexpected(std::move(r.error()));
        break;
    }
    return land;
  }

  /// Keyframe mode after positioning: feed the pending keyframe packet and drain, so the decoder
  /// emits that one frame at once instead of after a pipeline's worth of packets (frame threads
  /// hold ~thread_count packets). The decoder must be flushed before it is fed again
  /// (`frontier.drained`).
  [[nodiscard]] std::expected<void, Error> arm_keyframe_decode(const CancelToken& token) {
    if (!packets_.isPacketPending()) {
      // Positioned but not read yet: fetch the keyframe packet.
      auto land = read_landing(std::numeric_limits<std::int64_t>::max(), /*scan=*/false,
                               /*keyframe_mode=*/true, token);
      if (!land) return std::unexpected(std::move(land.error()));
      if (!packets_.isPacketPending()) return {};  // nothing to feed (EOF); select() reports it
    }
    if (!packets_.areKeyFlagsReliable() || packets_.getPacket().pts == k::no_pts)
      return {};  // cannot single out a keyframe: decode normally
    awaiting_key_ = false;
    const int s = send_held_packet();
    if (s < 0 && s != k::eagain)
      return fail(ErrorCode::decode_failed, s, "avcodec_send_packet (keyframe)");
    (void)avcodec_send_packet(codec_.get(), nullptr);
    frontier.draining = true;
    frontier.drained = true;
    landing_known_ = true;
    return {};
  }

  /// Reads the next packet of our stream and feeds it. Returns 0, k::eof (drain started),
  /// k::exit_requested (interrupted) or a decoder error.
  /// Not noexcept: the skipped-frame record below it allocates.
  [[nodiscard]] int feed_one(const CancelToken& token) {
    if (packets_.isPacketPending()) {
      if ((packets_.getPacket().flags & AV_PKT_FLAG_KEY) != 0) awaiting_key_ = false;
      prepare_send();
      const int s = send_held_packet();
      if (s == k::eagain) {
        // Both receive and send report EAGAIN: the decoder violates its contract.
        packets_.releasePacket();
        return k::einval;
      }
      if (s == 0) return 0;
      if (s != k::invalid_data) return s;
      ++total_decode_errors_;
      frontier.tainted = true;
    }
    if (frontier.drained)
      return k::eof;  // the decoder was drained for a keyframe-only decode; a seek resets it
    for (;;) {
      const int r = packets_.readVideoPacket(source_, keys_, frontier.tainted, token);
      if (r == k::exit_requested) return r;
      if (r == k::eof) {
        frontier.draining = true;
        (void)avcodec_send_packet(codec_.get(), nullptr);
        return k::eof;
      }
      const bool key_packet = (packets_.getPacket().flags & AV_PKT_FLAG_KEY) != 0;
      if (awaiting_key_ && packets_.areKeyFlagsReliable() && !key_packet) {
        // Mid-GOP after a seek: the decoder would decode these in full and drop them anyway.
        packets_.unrefPacket();
        continue;
      }
      if (key_packet) awaiting_key_ = false;
      prepare_send();
      const int s = send_held_packet();
      if (s == 0 || s == k::eagain) return 0;
      if (s == k::invalid_data) {
        ++total_decode_errors_;
        frontier.tainted = true;
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
    const AVPacket& pkt = packets_.getPacket();
    const bool key = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
    AVDiscard skip = AVDISCARD_DEFAULT;
    if (skip_before_ts_ != k::no_pts && packets_.areKeyFlagsReliable() && !key &&
        pkt.pts != k::no_pts && frontier.lastFedPts != k::no_pts) {
      const std::int64_t pts = pkt.pts;
      if (pts < frontier.lastFedPts && frontier.lastFedPts <= skip_before_ts_) {
        skip = AVDISCARD_NONREF;
        frontier.skipped.record(pts);  // if the decoder drops it, nothing may decode across it
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
        costs.noteFirstFrame();
        if (hw_active_) ++hw_frames_seen_;
        if (stream().synthesize_timestamps) {
          const std::int64_t stamped =
              frontier.synthTs != k::no_pts ? frontier.synthTs : stream().start_pts;
          recv_->pts = recv_->best_effort_timestamp = stamped;
          recv_->duration = stream().frame_duration_hint;
        }
        const std::int64_t t = frame_ts(*recv_);
        frontier.synthTs = t + frame_duration(*recv_);
        frontier.lastReceivedTs = t;
        ++frontier.receivedSinceSeek;
        const bool key = (recv_->flags & AV_FRAME_FLAG_KEY) != 0;
        // GOP length estimate (a lower bound, exact at the next keyframe): feeds the back-off step
        // and the forward-scan limit.
        keys_.noteKeyframeSpan(frontier.lastKeyTs, t);
        if (key) frontier.lastKeyTs = t;
        if ((recv_->flags & AV_FRAME_FLAG_CORRUPT) != 0) {
          frontier.skipped.record(t);  // not produced: nothing may decode across it
          // Concealed by construction: this is the frame the decoder itself flagged, stashed only
          // as a fallback for a stream that never produces a better one (finish_at_eof).
          corrupt_last_.adopt(*recv_, /*wasConcealed=*/true);
          continue;
        }
        // Corrupt: the decoder reported concealment, or a decode error occurred since the last
        // keyframe (the reference chain is suspect).
        if (key) frontier.tainted = false;
        const bool concealed = recv_->decode_error_flags != 0 || frontier.tainted;
        frontier.lastEnd = std::max(frontier.lastEnd, t + frame_duration(*recv_));
        frontier.skipped.clearAt(t);  // whatever was skipped here has now been produced
        if (keyframe_only_) {
          // "The keyframe at or before P": the fed keyframe after a packet-level landing
          // (landing_known_), or the first keyframe out after a seek aimed at P itself. After a
          // back-off (seek_target_ < P) that guarantee is gone: keep decoding, remembering the last
          // keyframe <= P, until a frame past P shows up.
          if (t <= P) {
            if (key || !held_.isValid())
              hold_received(concealed);  // a non-key frame only as a fallback
            if (key && (landing_known_ || t == P ||
                        (frontier.receivedSinceSeek == 1 && seek_target_ == P))) {
              return Selected{held_.getFrame(), Adjustment::none, held_.isConcealed()};
            }
            continue;
          }
          if (held_.isValid()) {
            pend_received(concealed);
            return Selected{held_.getFrame(), Adjustment::none, held_.isConcealed()};
          }
        } else {
          if (t < P) {
            hold_received(concealed);
            if (t >= lo)
              return Selected{held_.getFrame(), Adjustment::none,
                              held_.isConcealed()};  // early accept within tolerance
            continue;
          }
          if (t == P) {
            hold_received(concealed);
            return Selected{held_.getFrame(), Adjustment::none, held_.isConcealed()};
          }
          if (held_.isValid()) {
            pend_received(concealed);
            return Selected{held_.getFrame(), Adjustment::none, held_.isConcealed()};
          }
        }
        // Overshoot: the first frame out is already past P. Either P precedes the first frame of
        // the stream (provable only after the explicit start seek) or the seek landed late. A late
        // landing is backed off even when a later frame would satisfy the `after` tolerance: the
        // frame actually on screen at P exists until proven otherwise.
        const bool at_start = landed_at_start_ || !stream().seekable ||
                              (first_frame_ts_ != k::no_pts && t <= first_frame_ts_);
        const bool landing = frontier.receivedSinceSeek == 1;
        const std::int64_t observed_key = key ? t : k::no_pts;
        if (landing && !at_start) {
          if (auto s = back_off(P, observed_key); !s) return std::unexpected(std::move(s.error()));
          continue;
        }
        if (t <= hi) {
          hold_received(concealed);
          return Selected{held_.getFrame(), Adjustment::none, held_.isConcealed()};
        }
        if (at_start) {
          hold_received(concealed);
          return Selected{held_.getFrame(), Adjustment::clamped_to_first, held_.isConcealed()};
        }
        if (auto s = back_off(P, observed_key); !s) return std::unexpected(std::move(s.error()));
        continue;
      }
      if (r == k::eof) {
        // Nothing decodable between the landing and the end of the stream. Back off unless the
        // start seek has already been done, in which case the stream really has no frame for P.
        if (!held_.isValid() && !corrupt_last_.isValid() && stream().seekable && positioned_ &&
            !landed_at_start_ && !frontier.drained) {
          if (auto s = back_off(P, k::no_pts); !s) return std::unexpected(std::move(s.error()));
          continue;
        }
        // Not one frame came out of the decoder since this request positioned. On a seekable
        // source that says nothing about the stream: the start seek landed and the very first read
        // reported the end, which is what libavio's sticky eof_reached does after a cancellation
        // (a seek does not clear it). Clear it and re-position once before concluding the stream
        // ended — `landed_at_start_` above would otherwise accept that first read as proof.
        if (!held_.isValid() && !corrupt_last_.isValid() && stream().seekable &&
            !frontier.drained && frontier.receivedSinceSeek == 0 && !eof_retried_) {
          eof_retried_ = true;
          if (auto s = retry_after_empty_eof(P, token); !s)
            return std::unexpected(std::move(s.error()));
          continue;
        }
        frontier.eof = true;
        if (frontier.lastEnd != std::numeric_limits<std::int64_t>::min()) {
          tail_end_ =
              tail_end_ == k::no_pts ? frontier.lastEnd : std::max(tail_end_, frontier.lastEnd);
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
      frontier.tainted = true;
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
    const auto finish = [&](AVFrame* f, bool corrupt) -> std::expected<Selected, Error> {
      const std::int64_t t = frame_ts(*f);
      // In keyframe mode the stream extends past the held keyframe to the last frame decoded after
      // it.
      const std::int64_t end =
          std::max(t + frame_duration(*f), keyframe_only_ ? frontier.lastEnd : t);
      if (P < end) return Selected{f, Adjustment::none, corrupt};
      if (opt_.out_of_range == OutOfRangePolicy::clamp_to_last_frame)
        return Selected{f, Adjustment::clamped_to_last, corrupt};
      return fail(
          ErrorCode::time_out_of_range,
          "requested time is past the last frame (" +
              to_string(Time::from_timestamp(std::max<std::int64_t>(t - stream().start_pts, 0),
                                             from_av(stream().time_base))) +
              ")");
    };
    if (held_.isValid()) return finish(held_.getFrame(), held_.isConcealed());
    if (corrupt_last_.isValid()) {
      held_.adopt(corrupt_last_);  // carries the concealed flag the stash was adopted with
      // Nearest-keyframe mode asks for "the keyframe at or before P", and a stashed frame at or
      // before P answers that: its display interval is not the question, as it is in exact mode.
      if (keyframe_only_ && P >= frame_ts(*held_.getFrame()))
        return Selected{held_.getFrame(), Adjustment::none, true};
      return finish(held_.getFrame(), true);
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
    const std::int64_t gop = std::max({keys_.getGopHint(), frames_ticks(2), std::int64_t{1}});
    if (P <= t + gop || P <= packets_.getVerifiedTo()) return sel;  // plainly inside the data
    if (tail_end_ == k::no_pts) {
      // The demuxer sits just after the chosen keyframe: read on until the question is answered.
      for (;;) {
        const int r = packets_.readVideoPacket(source_, keys_, frontier.tainted, token);
        if (r == k::exit_requested) {
          if (token.requested()) return fail(ErrorCode::cancelled, "cancelled");
          return sel;  // an I/O hiccup is not proof of anything: keep the keyframe
        }
        if (r == k::eof) {
          const std::int64_t verified = packets_.getVerifiedTo();
          tail_end_ = verified != k::no_pts
                          ? verified + std::max<std::int64_t>(stream().frame_duration_hint, 1)
                          : t + std::max<std::int64_t>(stream().frame_duration_hint, 1);
          break;
        }
        packets_.unrefPacket();
        const std::int64_t verified = packets_.getVerifiedTo();
        if (verified != k::no_pts && verified > P) break;
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
            to_string(Time::from_timestamp(
                std::max<std::int64_t>(tail_end_ - stream().start_pts, 0),
                from_av(stream().time_base))) +
            ")");
  }

  /// Whether the held frame really is the last frame in presentation order of everything decoded
  /// since positioning. It is, for every monotonic stream. It is not on a container whose
  /// timestamps jump backwards (two recordings concatenated into one MPEG-TS, a camera restart):
  /// decoding forward across the jump ends with a frame from the *earlier* segment, and concluding
  /// "the stream has no frame past this one" from it strands the generator, because the
  /// end-of-stream shortcut then answers every later request without ever repositioning.
  [[nodiscard]] bool isHeldFrameAtTail() const noexcept {
    if (!held_.isValid()) return false;
    return frame_ts(*held_.getFrame()) + frame_duration(*held_.getFrame()) >= frontier.lastEnd;
  }

  /// Whether the held frame is the one on screen at `target`, as far as the frontier can say.
  ///
  /// The invariant: the held frame's display interval starts at or before `target` and every frame
  /// between the two was produced. A frame skipped in between is the one on screen instead, so the
  /// held frame does not cover `target` after all, whatever the look-ahead or the end of stream
  /// say. What rules out a *later* frame covering `target` is mode-specific and stays with the
  /// caller: the look-ahead frame or the end of stream in exact mode, the absence of a keyframe in
  /// (held, target] in nearest-keyframe mode.
  [[nodiscard]] bool isHeldFrameCovering(std::int64_t target) const noexcept {
    if (!held_.isValid()) return false;  // guards getFrame()
    const std::int64_t h = frame_ts(*held_.getFrame());
    return h <= target && !frontier.skipped.containsIn(h, target);
  }

  /// Whether the look-ahead frame can become the held frame and the decode continue from there.
  ///
  /// The invariant: the pending frame begins at or before `target`, so the frame on screen at
  /// `target` is it or one the decoder has yet to produce; every frame between it and `target` will
  /// be produced; and reaching `target` that way costs less than seeking to it.
  [[nodiscard]] bool canPromotePendingFrame(std::int64_t target) const noexcept {
    if (!pending_.isValid() || frontier.drained) return false;  // isValid() guards getFrame()
    const std::int64_t p = frame_ts(*pending_.getFrame());
    return target >= p && !frontier.skipped.containsIn(p, target) &&
           isForwardCheaperThanSeek(target);
  }

  /// Whether the request can be answered without repositioning, by decoding on from the held frame.
  ///
  /// The invariant: the decoder's next output is the frame after the held one, and every frame
  /// between the held frame and `target` will be produced. An empty look-ahead slot is what says
  /// nothing past the held frame has been received; `eof` and `drained` say the decoder has no more
  /// to give without a flush; a recorded hole says one of the frames in between was skipped and
  /// never will be produced. Any of those failing leaves the frame on screen at `target`
  /// unestablishable from here, and the request has to position.
  [[nodiscard]] bool canContinueFromHeldFrame(std::int64_t target) const noexcept {
    if (!held_.isValid() || pending_.isValid() || frontier.eof || frontier.drained) {
      return false;  // isValid() guards getFrame()
    }
    const std::int64_t h = frame_ts(*held_.getFrame());
    return h <= target && !frontier.skipped.containsIn(h, target) &&
           isForwardCheaperThanSeek(target);
  }

  /// Positions and selects (the caller converts).
  [[nodiscard]] std::expected<Selected, Error> extract(std::int64_t P, std::int64_t lo,
                                                       std::int64_t hi,
                                                       Adjustment clamped_by_bounds,
                                                       const CancelToken& token) {
    const bool infinite_before = lo == std::numeric_limits<std::int64_t>::min();
    // Nearest-keyframe mode (infinite `before`). Without a usable seek every frame is decoded
    // anyway, so fall back to exact selection instead of returning an arbitrary non-keyframe.
    const bool keyframe_mode = infinite_before && stream().seekable;
    if (infinite_before && !stream().seekable) {
      lo = P;
      hi = P;
    }
    // Fast path: the frame covering P is already held, and nothing later covers it instead.
    if (isHeldFrameCovering(P)) {
      if (keyframe_mode) {
        // The held frame is the keyframe covering P (the index says no keyframe lies in (h, P]).
        const std::int64_t h = frame_ts(*held_.getFrame());
        if ((held_.getFrame()->flags & AV_FRAME_FLAG_KEY) != 0 &&
            keys_.doesKeyCover(h, P, containerIndexOf(source_))) {
          return verify_keyframe_tail(
              Selected{held_.getFrame(), clamped_by_bounds, held_.isConcealed()}, P, token);
        }
      } else {
        if (pending_.isValid() && P < frame_ts(*pending_.getFrame())) {
          return Selected{held_.getFrame(), clamped_by_bounds, held_.isConcealed()};
        }
        if (frontier.eof && !pending_.isValid() && isHeldFrameAtTail()) {
          auto s = finish_at_eof(P);
          if (!s) return std::unexpected(std::move(s.error()));
          if (s->adjustment == Adjustment::none) s->adjustment = clamped_by_bounds;
          return *s;
        }
      }
    }
    keyframe_only_ = keyframe_mode;
    // Frames well before the tolerance window may skip their non-reference members. Off in
    // keyframe mode and on inputs without keyframe flags.
    skip_before_ts_ = k::no_pts;
    if (!keyframe_mode && packets_.areKeyFlagsReliable()) {
      const std::int64_t margin = keys_.getReorderTicks() + frames_ticks(2);
      skip_before_ts_ = lo > std::numeric_limits<std::int64_t>::min() + margin ? lo - margin : lo;
    }
    if (keyframe_mode) {
      if (auto r = position_for(P, true, token); !r) return std::unexpected(std::move(r.error()));
      const AVPacket& pkt = packets_.getPacket();
      const bool pre_edit =
          packets_.isPacketPending() &&
          ((pkt.flags & AV_PKT_FLAG_DISCARD) != 0 ||
           (first_frame_ts_ != k::no_pts && pkt.pts != k::no_pts && pkt.pts < first_frame_ts_));
      if (pre_edit) {
        // The keyframe covering P precedes the first presented frame (an edit list trimmed its
        // GOP). Answer with the first presented frame, clamped, decoded exactly: the decoder drops
        // the trimmed frames itself (AV_PKT_FLAG_DISCARD).
        keyframe_only_ = false;
        P = lo = hi = first_frame_ts_ != k::no_pts ? first_frame_ts_ : stream().start_pts;
        clamped_by_bounds = Adjustment::keyframe_before_edit;
      } else if (auto r = arm_keyframe_decode(token); !r) {
        return std::unexpected(std::move(r.error()));
      }
    } else if (canPromotePendingFrame(P)) {
      held_.adopt(pending_);
      backoffs_ = 0;
      backoff_step_ = 0;
    } else if (!canContinueFromHeldFrame(P)) {
      if (auto r = position_for(P, false, token); !r) return std::unexpected(std::move(r.error()));
    } else {
      backoffs_ = 0;
      backoff_step_ = 0;
    }
    costs.beginRequest();
    const int frames_before = cur_.frames_decoded;
    const bool seeked = getSeekCount() > 0;
    auto sel = select(P, lo, hi, token);
    if (!sel) {
      // A request abandoned before reaching its target fed packets under *its* skip window, and
      // only it knew where that window was: the frames between the decoder's frontier and the
      // window may never be produced, so nothing may continue forward across it. The next request
      // repositions — one seek per cancellation. A source that cannot be repositioned (a pipe)
      // relies on the recorded holes instead, and a request across one fails rather than lies.
      if (skip_before_ts_ != k::no_pts && stream().seekable) positioned_ = false;
      return std::unexpected(std::move(sel.error()));
    }
    costs.learn(cur_.frames_decoded - frames_before, seeked);
    if (sel->adjustment == Adjustment::none) sel->adjustment = clamped_by_bounds;
    if (keyframe_mode) return verify_keyframe_tail(*sel, P, token);
    return *sel;
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
    auto out = converter_->convert(*src, stream().container_sar, stream().codec_sar, req_max_);
    if (!out) return std::unexpected(std::move(out.error()));
    // A frame before the time origin (MPEG-TS whose start comes from another stream) is reported at
    // zero.
    const std::int64_t t = std::max<std::int64_t>(frame_ts(*sel.frame) - stream().start_pts, 0);
    const Time actual = Time::from_timestamp(t, from_av(stream().time_base));
    const bool key = (sel.frame->flags & AV_FRAME_FLAG_KEY) != 0;
    const ColorRange range = from_av(static_cast<AVColorRange>((*out)->color_range));
    const std::int64_t dur = frame_duration(*sel.frame);
    (*out)->duration = dur;
    Image img = ImageAccess::make(std::move(*out), opt_.pixel_format, range,
                                  actual.is_valid() ? actual : Time::zero(), key, sel.adjustment,
                                  sel.corrupt);
    if (dur > 0)
      ImageAccess::set_duration(img, Time::from_timestamp(dur, from_av(stream().time_base)));
    return img;
  }

  MediaSource source_;
  Options opt_;

  const AVCodec* codec_desc_{nullptr};
  CodecCtxPtr codec_;
  BufferRefPtr hw_device_;
  AVHWDeviceType hw_type_{AV_HWDEVICE_TYPE_NONE};
  std::unique_ptr<HwState> hw_state_;
  bool hw_active_{false};
  int hw_frames_seen_{0};
  bool hw_fault_{false};
  bool hw_retried_{false};  ///< the hardware decoder was rebuilt for the current run of failures

  AssetInfo info_;
  mutable std::mutex active_mutex_;
  ActiveDecoder active_;  // guarded by active_mutex_
  std::optional<Converter> converter_;

  KeyframeIndex keys_;  ///< where the keyframes are; the reader records into it as it reads
  PacketReader packets_;
  FramePtr recv_;  ///< scratch: avcodec_receive_frame writes here, then a slot adopts it
  FrameSlot held_, pending_, corrupt_last_;
  DecodeFrontier frontier;  ///< where the decoder is; every reposition resets it
  SeekCostModel costs;      ///< what a seek and a frame cost; drives isForwardCheaperThanSeek()
  const AVFrame* probe_frame_{nullptr};
  bool positioned_{false};
  bool landed_at_start_{false};  ///< the last positioning was the explicit start seek or a re-open
  bool awaiting_key_{false};     ///< no keyframe packet fed since the last positioning
  std::int64_t seek_target_{0};
  /// One re-position per request after an end of stream that decoded nothing (see select()).
  bool eof_retried_{false};
  bool keyframe_only_{false};  ///< the current request is in nearest-keyframe mode
  std::int64_t first_frame_ts_{k::no_pts};
  std::int64_t backoff_step_{0};
  int backoffs_{0};
  bool landing_known_{false};  ///< keyframe mode: the fed keyframe packet is the answer
  std::int64_t seek_bias_{0};  ///< subtracted from seek targets on demuxers that search a DTS index
                               ///< with an unshifted PTS (fragmented MP4)
  std::optional<Size> req_max_;  ///< RequestOptions::maximum_size of the current request
  std::int64_t skip_before_ts_{
      k::no_pts};  ///< packets whose frames end before this may skip non-reference frames
  std::int64_t tail_end_{k::no_pts};  ///< end of the data once the end of stream has been observed
                                      ///< (k::no_pts = not yet)
  int decode_errors_{0};
  long total_decode_errors_{0};
  std::string
      broken_reason_;  ///< why the last re-open failed (the pipeline retries on the next request)

  /// What the current request has cost so far, reset at the start of each one. Not observability:
  /// SeekCostModel::learn() reads frames_decoded, and isForwardCheaperThanSeek() decides
  /// seek-versus-decode-forward from the averages it maintains. Decoding thread only.
  struct RequestStats {
    /// MediaSource's lifetime seek count when this request started; getSeekCount() differences it.
    std::int64_t seeksAtStart{0};
    int frames_decoded{0};  ///< frames received from the decoder, look-ahead and skipped included
  };
  RequestStats cur_;
};

}  // namespace stills::detail

#pragma once
// stills/detail/media_source.hpp — the container and its I/O, and nothing that decodes.
//
// MediaSource owns the AVFormatContext, the chosen AVStream, and everything the pipeline learned
// about that stream at open time (StreamInfo). It opens and re-opens the source, issues the seek
// primitives, reads packets, and clears libavio's state after an interrupt. It does not own the
// decoder, the frames, the packet buffers or the position: those have different lifetimes and are
// the pipeline's.
//
// That lifetime difference is the reason this type exists. A hardware fallback rebuilds the
// decoder two or three times over one container, and the re-open path used to rebuild the decoder
// as a side effect of re-establishing the source — which is what made "opening the input" and
// "setting up the decoder" one inseparable blob. reopen() here re-opens the container and returns;
// the caller rebuilds whatever it needs afterwards, in the order it chooses.
//
// What deliberately stays out: probing the first frame, and the hardware-candidate loop that
// probes one decoder after another. Probing means running the real selection loop against the real
// source, which needs the decoder and the frame slots — so it belongs to the pipeline even though
// it is part of opening.
//
// Seek counting. Every avformat_seek_file / av_seek_frame this library issues goes through
// seekTo() or byteSeekTo(), and each increments a lifetime counter. A *request* is not a concept
// this type has, so it does not try to scope the count: the pipeline reads the counter when a
// request starts and again when it ends, and the difference is that request's seeks. The counter
// therefore cannot drift out of step with the calls, because it is incremented where they are
// made. The benchmark harness gates on that number (tests/bench).
//
// Not thread-safe: a Pipeline is single-threaded by contract (pipeline.hpp), and its MediaSource is
// only ever touched by the thread running that pipeline. The one exception is the interrupt
// callback, which libavformat calls on the same thread from inside the I/O it is blocking in.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "stills/detail/ffmpeg.hpp"
#include "stills/error.hpp"
#include "stills/options.hpp"

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

/// What the pipeline learned about the chosen video stream when the source was opened. Owned by
/// MediaSource, which is the only thing that may write it; everything else reads it through
/// MediaSource::getStreamInfo().
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

class MediaSource {
 public:
  /// What an interrupt left behind in libavio, once recoverAfterInterrupt() has done what it can.
  enum class IoState {
    clean,    ///< no interrupt state to clear; the source is readable as it stands
    cleared,  ///< the sticky end-of-file/error flags were cleared and the demuxer flushed
    stuck,    ///< this libavformat major cannot be cleared in place; the caller must re-open
  };

  explicit MediaSource(std::string sourcePath) : path(std::move(sourcePath)) {}

  // Neither copyable nor movable: libavformat holds `this` as the interrupt callback's opaque
  // pointer for the life of the AVFormatContext, so the object must not change address.
  MediaSource(const MediaSource&) = delete;
  MediaSource& operator=(const MediaSource&) = delete;
  MediaSource(MediaSource&&) = delete;
  MediaSource& operator=(MediaSource&&) = delete;

  ~MediaSource() = default;

  // Opens the container, picks the video stream and fills StreamInfo. Leaves the source closed on
  // failure, with one exception libavformat forces: avformat_open_input frees and nulls its context
  // itself, so a failure there has nothing to close.
  [[nodiscard]] std::expected<void, Error> open(const Options& opt) {
    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr) return fail(ErrorCode::out_of_memory, "avformat_alloc_context");
    raw->interrupt_callback.callback = &MediaSource::interruptCallback;
    raw->interrupt_callback.opaque = this;
    raw->flags |= AVFMT_FLAG_GENPTS;
    DictPtr dict;
    {
      AVDictionary* d = nullptr;
      for (const auto& [key, value] : opt.demuxer_options)
        av_dict_set(&d, key.c_str(), value.c_str(), 0);
      dict.reset(d);
    }
    AVDictionary* dict_raw = dict.release();
    // avformat_open_input frees and nulls `raw` on failure, so wrap only on success. Wrap it
    // *before* the unknown-option check: that check returns on a source that opened fine, and an
    // open context reached only through `raw` would be leaked.
    const int r = avformat_open_input(&raw, path.c_str(), nullptr, &dict_raw);
    dict.reset(dict_raw);  // whatever was not consumed
    if (r >= 0) fmt.reset(raw);
    if (auto bad = unknownDemuxerOptions(dict.get()); !bad.empty()) {
      return fail(ErrorCode::invalid_argument,
                  "demuxer_options: no such option in this FFmpeg build: " + bad +
                      " (a key libavformat knows but does not apply to this source is accepted)");
    }
    if (r < 0) {
      ErrorCode code = ErrorCode::open_failed;
      if (r == k::enoent) code = ErrorCode::file_not_found;
      if (r == k::invalid_data) code = ErrorCode::unsupported_format;
      return fail(code, r, "avformat_open_input(\"" + path + "\")");
    }

    // avformat_find_stream_info() decodes to fill in what the container did not declare, so an
    // oversized frame costs memory there too. Most containers (MP4, Matroska) declare the size in
    // their header: if every video stream already declares more than the cap, refuse before that.
    if (auto r2 = checkDeclaredSize(opt); !r2) return r2;
    if (int r2 = avformat_find_stream_info(fmt.get(), nullptr); r2 < 0) {
      return fail(ErrorCode::unsupported_format, r2, "avformat_find_stream_info");
    }

    const AVCodec* codec = nullptr;
    int idx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO,
                                  opt.video_stream_index.value_or(-1), -1, &codec, 0);
    if (idx == k::decoder_not_found) {
      return fail(ErrorCode::decoder_not_found, idx,
                  "av_find_best_stream: no decoder for the video stream");
    }
    if (idx < 0) {
      if (opt.video_stream_index) {
        return fail(ErrorCode::invalid_argument, idx,
                    "stream " + std::to_string(*opt.video_stream_index) +
                        " is not a decodable video stream");
      }
      return fail(ErrorCode::no_video_stream, idx,
                  "av_find_best_stream: no video stream in \"" + path + "\"");
    }
    // Cover art is a "video" stream to libavformat; not to us, unless asked for.
    if ((fmt->streams[idx]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0 &&
        !opt.allow_attached_pictures && !opt.video_stream_index) {
      int alt = -1;
      for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* s = fmt->streams[i];
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
      codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    }
    if (codec == nullptr) {
      return fail(ErrorCode::decoder_not_found, "no decoder available for the selected stream");
    }
    codecDesc = codec;
    st = fmt->streams[idx];
    if (opt.max_input_pixels) {
      // Before the decoder exists: setting it up and probing the first frame is what allocates for
      // the declared frame size, and a hostile file costs a few hundred bytes to declare it.
      const std::int64_t pixels = static_cast<std::int64_t>(std::max(st->codecpar->width, 0)) *
                                  std::max(st->codecpar->height, 0);
      if (pixels > *opt.max_input_pixels) {
        return fail(ErrorCode::unsupported_format,
                    "the video stream is " + std::to_string(st->codecpar->width) + "x" +
                        std::to_string(st->codecpar->height) + " = " + std::to_string(pixels) +
                        " pixels, over Options::max_input_pixels (" +
                        std::to_string(*opt.max_input_pixels) + ")");
      }
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
      if (static_cast<int>(i) != idx) fmt->streams[i]->discard = AVDISCARD_ALL;
    }

    stream = StreamInfo{};
    stream.index = idx;
    stream.time_base = st->time_base;
    if (stream.time_base.num <= 0 || stream.time_base.den <= 0) {
      return fail(ErrorCode::unsupported_format, "video stream has an invalid time base");
    }
    if (st->start_time != k::no_pts) {
      stream.start_pts = st->start_time;
    } else if (fmt->start_time != k::no_pts) {
      stream.start_pts = av_rescale_q(fmt->start_time, k::time_base_q, stream.time_base);
    }
    if (st->duration != k::no_pts && st->duration > 0) {
      stream.duration_pts = st->duration;
    } else if (fmt->duration != k::no_pts && fmt->duration > 0) {
      stream.duration_pts = av_rescale_q(fmt->duration, k::time_base_q, stream.time_base);
    }
    stream.avg_frame_rate = st->avg_frame_rate;
    const AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
    if (fr.num > 0 && fr.den > 0) {
      stream.frame_duration_hint = av_rescale_q(1, AVRational{fr.den, fr.num}, stream.time_base);
    }
    // Same priority as av_guess_sample_aspect_ratio (and therefore ffmpeg/ffplay): the container's
    // declaration wins over the bitstream's; a frame-level SAR sits in between (see convert()).
    stream.container_sar = st->sample_aspect_ratio;
    stream.codec_sar = st->codecpar->sample_aspect_ratio;
    stream.transform = readTransform(*st->codecpar);
    stream.io_seekable = fmt->pb != nullptr && (fmt->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    // Raw elementary streams (AVFMT_NOTIMESTAMPS) have nothing to seek by; libavformat's generic
    // seek fails and may leave the demuxer mid-file, so such inputs are decoded forward or
    // re-opened.
    stream.seekable = stream.io_seekable && (fmt->iformat->flags & AVFMT_NOTIMESTAMPS) == 0 &&
                       seekFailures < maxSeekFailures;
    stream.synthesize_timestamps = (fmt->iformat->flags & AVFMT_NOTIMESTAMPS) != 0;
    stream.index_trusted = avformat_index_get_entries_count(st) > 0;
    return {};
  }

  // Re-establishes the source from scratch: for a non-rewindable input that has to go back to the
  // start, and after a seek failed badly enough to give up on seeking.
  //
  // The time origin and the duration established by the first open are carried across. The re-probe
  // runs its I/O through the interrupt callback, so under a cancelled request it keeps only what it
  // managed to read: a duration short of the real one would then reject legitimate times for the
  // life of the generator. An interrupted probe under-reports the duration and never over-reports
  // it, so a longer answer is a source that genuinely grew and a shorter one is a truncated read.
  //
  // The caller checks isCancelled() before calling this, not this function: re-opening costs the
  // caller its decoder before the container is touched, and a cancelled request must not pay that.
  [[nodiscard]] std::expected<void, Error> reopen(const Options& opt) {
    const std::int64_t previousStartPts = stream.start_pts;
    const std::optional<std::int64_t> previousDuration = stream.duration_pts;
    const bool hadStream = st != nullptr;
    close();
    if (auto r = open(opt); !r) {
      close();
      return r;
    }
    // Restored before the caller rebuilds anything on top of this source. Nothing it runs in
    // between reads either field -- the decoder rebuild reads the time base, the frame-duration
    // hint and synthesize_timestamps, and the frontier reset reads none of StreamInfo -- so this
    // is the same value every later reader saw before. A rebuild that grew a read of start_pts
    // would make the two orderings differ, which is why it is stated rather than assumed.
    if (hadStream) {
      stream.start_pts = previousStartPts;
      if (previousDuration &&
          (!stream.duration_pts || *stream.duration_pts < *previousDuration)) {
        stream.duration_pts = previousDuration;
      }
    }
    return {};
  }

  // Releases the container. StreamInfo is left as it was: reopen() reads the time origin out of it
  // after closing, and a failed re-open leaves the pipeline reporting the duration it last knew.
  void close() noexcept {
    fmt.reset();
    st = nullptr;
    codecDesc = nullptr;
  }

  [[nodiscard]] bool isOpen() const noexcept { return static_cast<bool>(fmt); }
  [[nodiscard]] const StreamInfo& getStreamInfo() const noexcept { return stream; }
  // The chosen video stream, or nullptr while closed. Non-const because libavformat's index
  // accessors take it that way.
  [[nodiscard]] AVStream* getStream() const noexcept { return st; }
  // The decoder libavformat chose for that stream; the pipeline builds its AVCodecContext from it.
  [[nodiscard]] const AVCodec* getCodec() const noexcept { return codecDesc; }
  // The demuxer's name, or empty while closed or when libavformat gives none.
  [[nodiscard]] std::string getContainerName() const {
    if (!fmt || fmt->iformat == nullptr || fmt->iformat->name == nullptr) return {};
    return fmt->iformat->name;
  }

  // Neither the stream nor the container declared a start time (raw elementary streams), so
  // Time::zero() can only be anchored once the first frame has been decoded.
  [[nodiscard]] bool needsStartPtsFromFirstFrame() const noexcept {
    return st != nullptr && fmt != nullptr && st->start_time == k::no_pts &&
           fmt->start_time == k::no_pts;
  }
  // Anchors the time origin at a frame the caller decoded. See needsStartPtsFromFirstFrame().
  void setStartPts(std::int64_t pts) noexcept { stream.start_pts = pts; }

  // Takes the frame rate from the decoder, for a container that declared none of its own. Sets the
  // frame-duration hint and the reported average together so the two cannot disagree; the hint is
  // what the synthesised timestamps are spaced by.
  void adoptDecoderFrameRate(AVRational frameRate) noexcept {
    stream.frame_duration_hint =
        av_rescale_q(1, AVRational{frameRate.den, frameRate.num}, stream.time_base);
    stream.avg_frame_rate = frameRate;
  }

  // --- seeking ------------------------------------------------------------------------------

  // Seeks towards `ts`, returning libav's result. Where the seek actually landed is established
  // from the packets afterwards, never from this: a seek is a hint.
  [[nodiscard]] int seekTo(std::int64_t ts) noexcept {
    ++seekCalls;
    // min_ts = INT64_MIN, ts = max_ts: "the keyframe at or before ts", never later.
    return avformat_seek_file(fmt.get(), stream.index, std::numeric_limits<std::int64_t>::min(), ts,
                              ts, 0);
  }

  // Positions on a packet by byte offset (containers without a trusted index). The next packet read
  // is the one at `pos`.
  [[nodiscard]] int byteSeekTo(std::int64_t pos) noexcept {
    ++seekCalls;
    return av_seek_frame(fmt.get(), -1, pos, AVSEEK_FLAG_BYTE);
  }

  // Positions on the very first packet, unconditionally. A timestamp below the stream's first DTS
  // resolves to the first packet in libavformat's binary search (MPEG-TS/PS), to sample 0 in mov
  // and to the first cue in Matroska; index-based demuxers that reject an out-of-range timestamp
  // get a plain seek to start_pts instead, and if that fails too the caller re-opens.
  //
  // Both attempts count towards the seek counter, because both are work done against the
  // container. Returns libav's result for the last attempt made.
  [[nodiscard]] int seekToStart() noexcept {
    const std::int64_t margin = av_rescale_q(10, AVRational{1, 1}, stream.time_base);
    const std::int64_t floorTs = std::numeric_limits<std::int64_t>::min() / 2;
    const std::int64_t early =
        stream.start_pts - margin < floorTs ? floorTs : stream.start_pts - margin;
    int r = seekTo(early);
    if (r < 0 && r != k::exit_requested) r = seekTo(stream.start_pts);
    return r;
  }

  // Seeks made over this source's whole life, not per request. See the header comment.
  [[nodiscard]] std::int64_t getSeekCallCount() const noexcept { return seekCalls; }

  // A seek succeeded: the run of consecutive failures is over.
  void noteSeekSucceeded() noexcept { seekFailures = 0; }
  // A seek failed for a reason other than interrupted I/O. At the cap the source stops being
  // seekable for good, and the pipeline decodes forward or re-opens instead.
  void noteSeekFailed() noexcept {
    if (++seekFailures >= maxSeekFailures) stream.seekable = false;
  }

  // --- reading ------------------------------------------------------------------------------

  // Reads the next packet of *any* stream into `pkt`. Returns 0, k::eof, k::exit_requested or a
  // demux error. Filtering by stream index and deciding what a transient error means are the
  // caller's: this is the container's read, nothing more.
  [[nodiscard]] int readPacket(AVPacket& pkt) noexcept { return av_read_frame(fmt.get(), &pkt); }

  // Throws away the demuxer's buffered state. The decoder is not touched; the caller flushes it.
  void flushDemuxer() noexcept {
    if (fmt) avformat_flush(fmt.get());
  }

  // --- cancellation and I/O recovery ----------------------------------------------------------

  // The token libavformat's interrupt callback consults. Set for the duration of an operation that
  // may block on I/O and cleared afterwards, so a stale token can never abort a later request.
  void setCancelToken(const CancelToken* t) noexcept { token = t; }
  // Whether the operation in progress has been cancelled — the same answer the interrupt callback
  // gives libavformat.
  [[nodiscard]] bool isCancelled() const noexcept {
    return token != nullptr && token->requested();
  }

  /// An interrupt that fired inside libavformat I/O leaves the AVIOContext refusing to read. Clear
  /// it and flush the demuxer; the caller forgets its position, because the next request must seek
  /// (which resets the I/O layer properly) or, on a pipe, continue from where the read stopped.
  ///
  /// The state left behind is not reliably `error == AVERROR_EXIT` — the MPEG-TS demuxer turns the
  /// short read into AVERROR_EOF, and a later seek clears `error` but not `eof_reached`, which on
  /// its own is indistinguishable from a genuine EOF. So the interrupt is recorded when it fires
  /// (interruptCallback) rather than inferred here, and consumed either way.
  ///
  /// Returns `stuck` when the sticky state could not be cleared in place -- a libavformat major
  /// tryResetIoState() was never verified against -- and the source can be re-opened. The caller
  /// must then re-establish it before the next read; nothing else clears the I/O layer.
  [[nodiscard]] IoState recoverAfterInterrupt() noexcept {
    const bool fired = std::exchange(interruptFired, false);
    if (!fmt || fmt->pb == nullptr) return IoState::clean;
    if (fmt->pb->error != k::exit_requested && !(fired && fmt->pb->eof_reached != 0)) {
      return IoState::clean;
    }
    const bool cleared = tryResetIoState(*fmt->pb);
    avformat_flush(fmt.get());
    return cleared ? IoState::cleared : IoState::stuck;
  }

  // Clears libavio's sticky end-of-file / error state, for a caller that knows an end of stream was
  // spurious. True when there was nothing to clear or it was cleared; false only on a libavformat
  // major tryResetIoState() was never verified against, where the caller re-opens instead.
  [[nodiscard]] bool tryResetIo() noexcept {
    return !fmt || fmt->pb == nullptr || tryResetIoState(*fmt->pb);
  }

 private:
  /// Consecutive avformat_seek_file failures (never counting interrupted I/O) before the source
  /// stops seeking and the pipeline decodes forward / re-opens instead.
  static constexpr int maxSeekFailures = 3;

  static int interruptCallback(void* opaque) noexcept {
    auto* self = static_cast<MediaSource*>(opaque);
    if (self == nullptr) return 0;
    if (self->token != nullptr && self->token->requested()) {
      // Recorded here because libavio's own state does not preserve it: see
      // recoverAfterInterrupt(). Same thread as the request that is being interrupted.
      self->interruptFired = true;
      return 1;
    }
    return 0;
  }

  /// Demuxer options libavformat left unconsumed *and* does not define anywhere: a misspelling.
  /// A key it defines but did not apply here is legitimate — an HTTP option that a local path
  /// never reaches — and is not reported. Returns them comma-separated, or empty.
  [[nodiscard]] static std::string unknownDemuxerOptions(AVDictionary* left) {
    std::string bad;
    const AVDictionaryEntry* e = nullptr;
    while ((e = av_dict_iterate(left, e)) != nullptr) {
      if (optionExists(e->key)) continue;
      if (!bad.empty()) bad += ", ";
      bad += '"';
      bad += e->key;
      bad += '"';
    }
    return bad;
  }

  /// Whether any of libavformat's option classes (the context, the demuxers, the protocols)
  /// defines `key`.
  [[nodiscard]] static bool optionExists(const char* key) noexcept {
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
  /// is unknown here is decided by the check in open() once stream info has been read.
  [[nodiscard]] std::expected<void, Error> checkDeclaredSize(const Options& opt) const {
    if (!opt.max_input_pixels) return {};
    int worst_w = 0, worst_h = 0;
    bool any_video = false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
      const AVCodecParameters& par = *fmt->streams[i]->codecpar;
      if (par.codec_type != AVMEDIA_TYPE_VIDEO) continue;
      any_video = true;
      const std::int64_t pixels =
          static_cast<std::int64_t>(std::max(par.width, 0)) * std::max(par.height, 0);
      if (pixels <= *opt.max_input_pixels) return {};  // one of them might be usable
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
                    std::to_string(*opt.max_input_pixels) + ")");
  }

  /// The display transform from the stream's display matrix side data (codecpar->coded_side_data,
  /// the non-deprecated path on FFmpeg 6.1 and 7.x).
  [[nodiscard]] static DisplayTransform readTransform(const AVCodecParameters& par) noexcept {
    const AVPacketSideData* sd = av_packet_side_data_get(
        par.coded_side_data, par.nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (sd == nullptr || sd->size < 9 * sizeof(std::int32_t)) return DisplayTransform{};
    std::int32_t matrix[9];
    std::memcpy(matrix, sd->data, sizeof matrix);
    return decode_display_matrix(matrix);
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

  std::string path;
  FormatCtxPtr fmt;
  AVStream* st{nullptr};
  const AVCodec* codecDesc{nullptr};  ///< the decoder av_find_best_stream picked, not a context
  StreamInfo stream;
  /// Set by interruptCallback() when it aborts libav I/O, consumed by recoverAfterInterrupt().
  bool interruptFired{false};
  int seekFailures{0};  ///< consecutive avformat_seek_file failures (interrupts excluded)
  /// Seeks issued over this source's whole life; the pipeline differences it per request.
  std::int64_t seekCalls{0};
  const CancelToken* token{nullptr};
};

}  // namespace stills::detail

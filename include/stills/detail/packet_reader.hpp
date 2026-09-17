#pragma once
// stills/detail/packet_reader.hpp — the packet pump over a MediaSource, and the packets it owns.
//
// PacketReader owns the one AVPacket every read lands in, the parking slot the keyframe scan uses,
// and the GOP replay buffer. It filters the chosen stream out of the demuxer's output, absorbs
// transient demux errors, learns whether the demuxer's keyframe flags can be trusted at all, and
// remembers how far into the stream packets have actually been read. It does not decode, does not
// seek, and does not decide where to position: it is asked for the next packet of the stream and
// produces one.
//
// Why it is its own type. The replay buffer is byte-level bookkeeping — a bounded vector of
// AVPackets scanned past a keyframe so the decoder can be fed them again without re-reading the
// container. That belongs next to the packet it replays into, not inside a seek strategy and not
// inside the decode loop. Packet *ownership* is the same argument from the other side: exactly one
// object may av_packet_move_ref into the live packet and exactly one may unref it, and until now
// eleven functions across the pipeline did both.
//
// The packet's lifecycle. getPacket() hands out the live packet by reference; whoever holds it may
// read it and must then either holdPacketForRetry() (the decoder refused it with EAGAIN and it
// must be sent again unchanged) or releasePacket() (it has been consumed). A caller that only
// looked at the packet drops it with unrefPacket(), which leaves the flag alone. The "pending"
// flag and the packet are one thing: a packet is pending exactly when the reader is holding it
// for a retry, and resetPosition() is the only other thing that may drop it.
//
// Not thread-safe: a Pipeline is single-threaded by contract (pipeline.hpp), and its PacketReader
// is only ever touched by the thread running that pipeline.

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>
#include <vector>

#include "stills/detail/ffmpeg.hpp"
#include "stills/detail/keyframe_index.hpp"
#include "stills/detail/media_source.hpp"
#include "stills/error.hpp"

namespace stills::detail {

class PacketReader {
 public:
  /// Allocates the live packet and the parking slot. Runs after every MediaSource open and
  /// re-open, because both belong to the run over one container.
  ///
  /// It does *not* clear the replay buffer, matching what the pipeline did before this type
  /// existed: reopen() does not reset the position either, and every path that re-opens reaches
  /// readLanding() next, whose first act is clearGopBuffer(). Now that one type owns the buffer
  /// that is a guarantee worth making here instead of relying on the caller -- but it would be a
  /// behaviour change, so it belongs to the step that owns re-opening, not to this one.
  [[nodiscard]] std::expected<void, Error> attach() {
    auto p = make_packet();
    if (!p) return std::unexpected(p.error());
    pkt = std::move(*p);
    pktPending = false;
    auto lp = make_packet();
    if (!lp) return std::unexpected(lp.error());
    landPkt = std::move(*lp);
    return {};
  }

  /// Drops everything tied to where the demuxer was: the replay buffer, the packet held for a
  /// retry, the parked keyframe and the run of absorbed demux errors. What the reader has learned
  /// about the *stream* (whether keyframe flags are reliable, how far packets have been read)
  /// deliberately survives: a reposition does not un-read what was read.
  void resetPosition() noexcept {
    clearGopBuffer();
    demuxErrors = 0;
    if (pktPending && pkt) av_packet_unref(pkt.get());
    pktPending = false;
    if (landPkt) av_packet_unref(landPkt.get());
  }

  /// A re-opened (or grown) source may reach further than the old one did, so what was read from
  /// the old one proves nothing about the new one.
  void resetVerifiedTo() noexcept { verifiedTo = k::no_pts; }

  /// The live packet. Valid from attach() onwards; the pipeline has none before it opens.
  [[nodiscard]] AVPacket& getPacket() noexcept { return *pkt; }
  [[nodiscard]] const AVPacket& getPacket() const noexcept { return *pkt; }

  /// True when the live packet is being held for a retry: it has not been consumed and must be
  /// sent again unchanged.
  [[nodiscard]] bool isPacketPending() const noexcept { return pktPending; }

  /// The decoder refused the packet (EAGAIN): keep it, unchanged, for the next send.
  void holdPacketForRetry() noexcept { pktPending = true; }

  /// The packet has been consumed, or the retry is being abandoned: unref it and stop holding it.
  void releasePacket() noexcept {
    av_packet_unref(pkt.get());
    pktPending = false;
  }

  /// Drops the live packet's contents and deliberately leaves the pending flag as it was: the
  /// caller read this packet only to look at it, and whether a *retry* is owed for some earlier
  /// packet is a separate question it has no business answering.
  ///
  /// The distinction from releasePacket() is not cosmetic. At the keyframe-tail check
  /// (Pipeline::verify_keyframe_tail) the flag can still be set from a send that returned EAGAIN,
  /// over a packet this very loop has already overwritten. Clearing it there would be a behaviour
  /// change, not a tidy-up: what makes that state safe is that the loop ends with
  /// `positioned_ = false`, so the next request repositions and reset_position() clears the flag
  /// before anything could send the packet. Do not collapse the two without moving that guarantee.
  void unrefPacket() noexcept { av_packet_unref(pkt.get()); }

  /// Whether the demuxer flags keyframe packets at all. Learned from the very first packet of the
  /// stream: a demuxer that never sets the flag must not make the pipeline skip.
  [[nodiscard]] bool areKeyFlagsReliable() const noexcept { return keyFlagsReliable; }

  /// The largest packet presentation time actually read from the source, k::no_pts if none. This
  /// is evidence that the stream reaches that far, which a decoded frame alone is not.
  [[nodiscard]] std::int64_t getVerifiedTo() const noexcept { return verifiedTo; }

  /// Reads the next packet of our stream into the live packet (other streams are dropped), and
  /// tells the keyframe index about it. Returns 0, k::eof (after a live-source growth check),
  /// k::exit_requested or a demux error (transient ones are skipped up to a limit).
  ///
  /// `tainted` is set — never cleared — when a demux error forced packets to be skipped: there is
  /// a hole in what the decoder will be fed, and the caller owns the state that records it.
  ///
  /// Not noexcept: the keyframe index this records into allocates. A std::bad_alloc here would be
  /// std::terminate rather than the ErrorCode::out_of_memory the API can report.
  [[nodiscard]] int readVideoPacket(MediaSource& source, KeyframeIndex& keys, bool& tainted,
                                    const CancelToken& token) {
    if (isServingReplay()) {
      // Replayed packets were recorded when they were first read; recording one twice would drag
      // the index's contiguity cursor backwards.
      // Packets scanned past the chosen keyframe are replayed; the demuxer sits right after them.
      av_packet_unref(pkt.get());
      av_packet_move_ref(pkt.get(), gopBuffer[replayPos].get());
      if (++replayPos == gopBuffer.size()) clearGopBuffer();
      return 0;
    }
    const int index = source.getStreamInfo().index;
    for (;;) {
      if (token.requested()) return k::exit_requested;
      int r = source.readPacket(*pkt);
      if (r == k::exit_requested) return r;
      if (r == k::eof || (r < 0 && ++demuxErrors > max_consecutive_demux_errors)) return k::eof;
      if (r < 0) {
        tainted = true;
        continue;  // transient demux error: skip and keep reading
      }
      demuxErrors = 0;
      if (pkt->stream_index == index && pkt->pts != k::no_pts) {
        verifiedTo = verifiedTo == k::no_pts ? pkt->pts : std::max(verifiedTo, pkt->pts);
      }
      if (pkt->stream_index != index) {
        av_packet_unref(pkt.get());
        continue;
      }
      if (!firstPacketSeen) {
        // Trust the demuxer's keyframe flags only if it flags the very first packet; a demuxer
        // that never sets the flag must not make us skip.
        firstPacketSeen = true;
        keyFlagsReliable = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
      }
      keys.notePacket(*pkt, containerIndexOf(source));
      return 0;
    }
  }

  /// What readLanding() established, plus the two things only the caller may act on.
  struct Landing {
    /// How the caller must get back to the chosen keyframe before it can be decoded. The scan
    /// necessarily reads past that keyframe, so when its GOP was too large to keep in memory
    /// someone has to go back to it — and seeking belongs to positioning, not to the reader.
    enum class Rewind { none, byteSeek, timestampSeek };

    bool found{false};  ///< a keyframe packet at or before P is ready (pending or positioned)
    std::int64_t first_key_pts{k::no_pts};  ///< the first keyframe packet seen (> P when !found)
    std::int64_t first_key_dts{k::no_pts};
    bool eof{false};
    /// The chosen keyframe's GOP is buffered for replay, so the next packet the decoder sees is
    /// that keyframe: the caller's positioning state must say a keyframe is still awaited.
    bool awaitKey{false};
    Rewind rewind{Rewind::none};
    KeyEntry rewindEntry{};            ///< Rewind::byteSeek: the keyframe to seek back to
    std::int64_t rewindTs{k::no_pts};  ///< Rewind::timestampSeek: the timestamp to aim at
  };

  /// Establishes where a seek landed from the packets, without decoding.
  ///
  /// Two of its outcomes are the caller's to carry out and are returned rather than performed:
  /// `rewind` (a seek, which belongs to positioning) and `awaitKey` (the buffered GOP starts at
  /// the chosen keyframe, so the caller's positioning state must still say one is awaited).
  /// Ignoring either would feed the decoder a mid-GOP packet stream. Pipeline::read_landing() is
  /// the one caller and does both; anything else that calls this owes them too.
  /// `scan == false` (trusted
  /// index): the first keyframe packet is the landing; it is held for the decoder when it
  /// is at or before P. `scan == true` (no index): keeps reading through the GOPs up to P, records
  /// every keyframe, and settles on the last keyframe at or before P — held for a
  /// keyframe-only decode, or reached again with a byte seek for an exact decode.
  [[nodiscard]] std::expected<Landing, Error> readLanding(MediaSource& source, KeyframeIndex& keys,
                                                          std::int64_t P, bool scan,
                                                          bool keyframe_mode, bool& tainted,
                                                          const CancelToken& token) {
    const ContainerIndex container = containerIndexOf(source);
    const std::int64_t hint = source.getStreamInfo().frame_duration_hint;
    const std::int64_t one_frame = hint > 0 ? hint : std::int64_t{0};
    Landing L;
    std::int64_t best_pts = k::no_pts, best_dts = k::no_pts, best_pos = -1;
    bool best_held = false;  // keyframe mode: the best packet is parked in the reader
    bool buffering = false;  // exact mode: packets after the best keyframe are kept for replay
    bool scanning = scan;    // becomes true on a trusted-index container whose seek undershot
    std::int64_t landing_pts = k::no_pts;  // first packet read after the seek
    // The keyframe-only arming path passes P = INT64_MAX ("the next keyframe, wherever
    // it is"): saturate.
    std::int64_t horizon = 0;
    if (__builtin_add_overflow(P, keys.getReorderTicks() + one_frame, &horizon)) {
      horizon = std::numeric_limits<std::int64_t>::max();
    }
    clearGopBuffer();
    for (;;) {
      const int r = readVideoPacket(source, keys, tainted, token);
      if (r == k::exit_requested) {
        if (token.requested()) return fail(ErrorCode::cancelled, "cancelled");
        return fail(ErrorCode::decode_failed, r,
                    "av_read_frame: interrupted without a cancellation");
      }
      if (r == k::eof) {
        L.eof = true;
        break;
      }
      if (!areKeyFlagsReliable()) {
        // Cannot tell keyframes apart at the packet level: feed everything, the decoder sorts it
        // out.
        holdPacketForRetry();
        L.found = true;
        return L;
      }
      const AVPacket& pkt = *this->pkt;
      const bool key = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
      const std::int64_t pts =
          pkt.pts != k::no_pts
              ? pkt.pts
              // Read fresh: notePacket() may have just lowered the delay from this very packet.
              : (pkt.dts != k::no_pts ? pkt.dts + keys.getReorderTicks() : k::no_pts);
      if (landing_pts == k::no_pts && pts != k::no_pts) landing_pts = pts;
      if (!key) {
        if (buffering) {
          // Part of the chosen keyframe's GOP: keep it instead of reading it twice.
          const bool past = pts != k::no_pts && pts > horizon;
          if (!bufferPacket()) buffering = false;  // over the cap: fall back to a byte seek
          if (scanning && best_pts != k::no_pts && past)
            break;  // every later packet is past P in decode order too
          continue;
        }
        // In keyframe mode the scan runs on to the next keyframe so the chosen one's GOP extent is
        // recorded and later requests inside it are answered from the held frame.
        if (scanning && !keyframe_mode && best_pts != k::no_pts && pts != k::no_pts &&
            pts > horizon) {
          unrefPacket();
          break;
        }
        unrefPacket();
        continue;
      }
      if (pts == k::no_pts) {
        holdPacketForRetry();  // a keyframe without a timestamp: nothing to verify against
        L.found = true;
        return L;
      }
      if (L.first_key_pts == k::no_pts) {
        L.first_key_pts = pts;
        L.first_key_dts = pkt.dts;
        // No keyframe between the landing and this one: the GOP is at least that long.
        keys.noteKeyframeSpan(landing_pts, pts);
      }
      if (pts > P) {
        if (best_pts != k::no_pts) {
          // The keyframe before this one is the answer; this packet is the next in decode order.
          if (buffering && !bufferPacket())
            buffering = false;
          else if (!buffering)
            unrefPacket();
          break;
        }
        unrefPacket();
        return L;  // overshoot: nothing at or before P was seen
      }
      if (!scanning) {
        if (!keys.hasKeyBetween(pts, P, container)) {
          holdPacketForRetry();  // the landing keyframe covers P
          L.found = true;
          return L;
        }
        // The index records a later keyframe at or before P (mov lands a GOP early on edit-list
        // files). Read on like a scan: a keyframe past P ends it with the last one at or before P,
        // so a wrong index entry cannot make us overshoot.
        scanning = true;
      }
      best_pts = pts;
      best_dts = pkt.dts;
      best_pos = pkt.pos;
      if (keyframe_mode) {
        parkPacket();
        best_held = true;
      } else {
        clearGopBuffer();
        buffering = bufferPacket();  // the keyframe itself is the first packet to feed
      }
    }
    if (best_pts == k::no_pts) {
      clearGopBuffer();
      return L;  // tail of the stream without a keyframe, or nothing at all
    }
    if (L.eof) {
      // The stream ended inside this GOP: its extent is known.
      keys.noteStreamEndedInGop(best_pts);
    }
    L.found = true;
    if (keyframe_mode && best_held) {
      takeParkedPacket();
      return L;
    }
    if (buffering && hasBufferedGop()) {
      // The chosen keyframe and its GOP are buffered: decoding replays them.
      L.awaitKey = true;
      beginReplay();
      return L;
    }
    clearGopBuffer();
    // GOP too large to buffer: go back to the keyframe by byte position (trusted-index containers
    // refuse byte seeks and re-seek by timestamp below).
    if (best_pos >= 0 && !container.trusted) {
      const KeyEntry* recorded = keys.findEntry(best_pts);
      L.rewind = Landing::Rewind::byteSeek;
      L.rewindEntry =
          recorded != nullptr ? *recorded : KeyEntry{best_pts, best_dts, best_pos, k::no_pts};
      L.found = true;
      return L;
    }
    // No byte position (unusual): re-seek by timestamp.
    L.rewind = Landing::Rewind::timestampSeek;
    L.rewindTs = best_pts - keys.getReorderTicks();
    return L;
  }

 private:
  /// True when the next read will be served from the replay buffer rather than the demuxer.
  [[nodiscard]] bool isServingReplay() const noexcept {
    return replaying && replayPos < gopBuffer.size();
  }

  /// Keeps the live packet for replay (the landing scan). False when the cap is exceeded, which
  /// also empties the buffer: a partial GOP is worse than none, because replaying it would feed
  /// the decoder frames whose references were never sent.
  [[nodiscard]] bool bufferPacket() {
    const std::size_t bytes = static_cast<std::size_t>(std::max(pkt->size, 0));
    if (gopBufferBytes + bytes > max_gop_buffer_bytes) {
      av_packet_unref(pkt.get());
      clearGopBuffer();
      return false;
    }
    PacketPtr p{av_packet_alloc()};
    if (!p) {
      av_packet_unref(pkt.get());
      clearGopBuffer();
      return false;
    }
    av_packet_move_ref(p.get(), pkt.get());
    gopBuffer.push_back(std::move(p));
    gopBufferBytes += bytes;
    return true;
  }

  void clearGopBuffer() noexcept {
    gopBuffer.clear();
    gopBufferBytes = 0;
    replayPos = 0;
    replaying = false;
  }

  /// Whether the scan buffered anything to replay.
  [[nodiscard]] bool hasBufferedGop() const noexcept { return !gopBuffer.empty(); }

  /// The scan is over and the buffered GOP is the answer: reads are served from it until it runs
  /// out, and the demuxer picks up exactly where the scan left it.
  void beginReplay() noexcept { replaying = true; }

  /// Parks the live packet (keyframe mode: the chosen keyframe, while the scan reads on past it).
  void parkPacket() noexcept {
    av_packet_unref(landPkt.get());
    av_packet_move_ref(landPkt.get(), pkt.get());
  }

  /// Makes the parked keyframe the live packet again, held for the decoder.
  void takeParkedPacket() noexcept {
    av_packet_unref(pkt.get());
    av_packet_move_ref(pkt.get(), landPkt.get());
    pktPending = true;
  }

  /// Scanned packets of the chosen GOP are kept for replay up to this much; beyond it (4K at high
  /// bit rates) the pipeline goes back to the keyframe with a byte seek instead.
  static constexpr std::size_t max_gop_buffer_bytes = 64u << 20;
  /// Matches the decoder's own limit in pipeline.hpp; the two count unrelated things.
  static constexpr int max_consecutive_demux_errors = 32;

  PacketPtr pkt;
  bool pktPending{false};  ///< pkt holds a packet the decoder refused with EAGAIN
  PacketPtr landPkt;       ///< keyframe mode: the chosen keyframe packet while scanning past it
  std::vector<PacketPtr> gopBuffer;  ///< exact mode: the chosen keyframe's packets scanned past P,
                                     ///< replayed to the decoder
  std::size_t gopBufferBytes{0};
  std::size_t replayPos{0};
  bool replaying{false};  ///< the scan is over; readVideoPacket serves gopBuffer first
  int demuxErrors{0};
  bool firstPacketSeen{false};
  bool keyFlagsReliable{false};  ///< the demuxer flags keyframe packets (first packet was flagged)
  std::int64_t verifiedTo{k::no_pts};  ///< largest packet presentation time actually read
};

}  // namespace stills::detail

#pragma once
// stills/detail/decode_frontier.hpp — how far the decoder has got, and what it never produced.
//
// DecodeFrontier is the state five jobs in the pipeline share: what has been fed to the decoder,
// what has come back out, whether the stream ended, and which frames were skipped on purpose.
// Positioning reads it to choose seek-versus-decode-forward, selection writes it, recovery
// invalidates it. Naming it is what lets those become separate types later without back-pointers:
// the decode loop owns it and the positioner takes it as a `const&`, so "reads the frontier" and
// "writes the frontier" are visible in the signatures rather than implied by comments.
//
// Everything in it is invalidated together, because a reposition makes all of it describe a place
// the decoder no longer is. That is what reset() is, and why the fields are one struct rather than
// twelve FramePipeline members. One caller contradicts that on purpose: recover_after_interrupt()
// carries lastReceivedTs across the reset, because a source that cannot be rewound really is still
// at or after that frame. It is the only such exception and it says so at the call site.
//
// A struct, not a class: centralising invalidation is what this type is for, and no two fields
// here have to agree with each other. FrameSlot is a class because two of its do.
//
// Not thread-safe: a FramePipeline is single-threaded by contract (pipeline.hpp), and the
// frontier is only ever touched by the thread running that pipeline's decode loop.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "stills/detail/ffmpeg.hpp"

namespace stills::detail {

// Presentation times of frames that were never produced: skipped at the decoder level
// (AVDISCARD_NONREF, for a request whose window was far ahead of them) or received flagged corrupt.
//
// A hole between the decoder's frontier and a request means the frame that belongs on screen was
// never decoded, so nothing may be concluded across one — not the held frame covering the request,
// not the decision to continue forward instead of seeking.
//
// Invariant, stated once here rather than at each of the call sites: `holes` is sorted ascending,
// holds no duplicates, contains no k::no_pts, and never grows past maxHoles.
class SkippedFrames {
 public:
  // Records a frame that may never be produced. A request abandoned before its target can leave a
  // gap ahead of the decoder's frontier, which is why these outlive the request that caused them.
  void record(std::int64_t pts) {
    if (pts == k::no_pts) return;
    const auto it = std::lower_bound(holes.begin(), holes.end(), pts);
    if (it != holes.end() && *it == pts) return;
    if (holes.size() >= maxHoles) {
      holes.clear();  // pathological: forget everything rather than grow without bound
      return;
    }
    holes.insert(it, pts);
  }

  // The frame was decoded after all: it is no longer a gap.
  void clearAt(std::int64_t pts) noexcept {
    if (holes.empty() || pts == k::no_pts) return;
    const auto it = std::lower_bound(holes.begin(), holes.end(), pts);
    if (it != holes.end() && *it == pts) holes.erase(it);
  }

  // True when a skipped or undecoded frame lies in (after, upTo].
  [[nodiscard]] bool containsIn(std::int64_t after, std::int64_t upTo) const noexcept {
    if (holes.empty() || upTo <= after) return false;
    const auto it = std::upper_bound(holes.begin(), holes.end(), after);
    return it != holes.end() && *it <= upTo;
  }

  // Forgets every hole. Nothing survives a reposition, so no gap recorded before one can matter.
  void clear() noexcept { holes.clear(); }

 private:
  static constexpr std::size_t maxHoles = 1u << 14;
  std::vector<std::int64_t> holes;
};

struct DecodeFrontier {
  // Timestamps are in the stream's own time base; k::no_pts means "nothing yet".
  std::int64_t lastFedDts = k::no_pts;  // dts of the last packet sent to the decoder
  std::int64_t lastFedPts = k::no_pts;  // largest pts sent to the decoder since positioning
  std::int64_t lastReceivedTs = k::no_pts;
  std::int64_t lastKeyTs = k::no_pts;
  // Furthest frame end seen since positioning. Not lastReceivedTs plus a duration: a stream whose
  // timestamps jump backwards can receive an earlier frame after a later one.
  std::int64_t lastEnd = std::numeric_limits<std::int64_t>::min();
  // Output-order counter for containers that carry no timestamps of their own (raw elementary
  // streams), and the fallback timestamp for a frame that has none.
  std::int64_t synthTs = k::no_pts;
  int receivedSinceSeek = 0;  // frames out of the decoder since the last positioning
  bool eof = false;
  bool draining = false;
  // The decoder was drained for a keyframe-only decode and must be flushed (seek) before more
  // input. Distinct from `draining`, which is the drain in progress.
  bool drained = false;
  bool tainted = false;  // a decode error occurred since the last keyframe
  SkippedFrames skipped;

  // Invalidates the whole frontier. Called wherever the decoder is flushed or the demuxer moved:
  // nothing decoded before that describes where the decoder is now.
  void reset() noexcept {
    lastFedDts = lastFedPts = k::no_pts;
    lastReceivedTs = k::no_pts;
    lastKeyTs = k::no_pts;
    lastEnd = std::numeric_limits<std::int64_t>::min();
    synthTs = k::no_pts;
    receivedSinceSeek = 0;
    eof = draining = drained = false;
    tainted = false;
    skipped.clear();
  }
};

}  // namespace stills::detail

// Direct unit tests for stills::detail::KeyframeIndex — the first detail:: type the suite tests
// on its own rather than through the public API.
//
// Everything here runs against the *recorded* index: keyframes learned from packets on a container
// libavformat did not index (MPEG-TS). That half needs no AVFormatContext, no file and no decoder,
// which is why ContainerIndex is a per-call parameter rather than a reference to the source — a
// ContainerIndex with `trusted == false` is a complete, valid container for these queries and
// nothing below ever touches its (null) AVStream. The container-index half is exercised by the
// integration tests over real MP4 and Matroska fixtures, because it is libavformat's index that is
// under test there, and a fake one would test nothing.
//
// What these pin is the contract, not the implementation: a GOP's extent is unknown until the
// packets up to the next keyframe have been read *contiguously*, a reposition breaks that chain,
// the estimate of GOP length only grows, and the reorder delay is the smallest a keyframe has ever
// shown. Those are the properties positioning relies on; the binary search and the cursor
// arithmetic are only visible here through them.

#include <cstdint>
#include <limits>
#include <stills/detail/keyframe_index.hpp>

#include "support/common.hpp"

using stills::detail::ContainerIndex;
using stills::detail::KeyEntry;
using stills::detail::KeyframeIndex;
namespace k = stills::detail::k;

namespace {

/// A container whose own index is not to be consulted: the MPEG-TS case, and the only one that can
/// be built without opening a file.
constexpr ContainerIndex scanned{};
/// A container libavformat indexed at open. No AVStream, so only the paths that stop at the
/// `trusted` flag may be reached with it.
constexpr ContainerIndex indexed{nullptr, true, 0};

/// A keyframe packet as the index sees one: it reads four fields and never touches the payload.
AVPacket key_packet(std::int64_t pts, std::int64_t dts, std::int64_t pos) {
  AVPacket p{};
  p.flags = AV_PKT_FLAG_KEY;
  p.pts = pts;
  p.dts = dts;
  p.pos = pos;
  return p;
}

}  // namespace

TEST_CASE("KeyframeIndex: a recorded keyframe covers nothing until its GOP extent is known",
          "[keyframe_index]") {
  KeyframeIndex idx;
  idx.recordKey(0, 0, 100, scanned);
  // One keyframe proves only where it starts. Answering "this GOP covers P" from it would claim
  // the rest of the stream.
  CHECK(idx.findCoveringKey(0) == nullptr);
  CHECK(idx.findEntry(0) != nullptr);  // recorded, just not yet bounded

  idx.recordKey(1000, 1000, 200, scanned);
  REQUIRE(idx.findCoveringKey(0) != nullptr);
  CHECK(idx.findCoveringKey(0)->pts == 0);
  REQUIRE(idx.findCoveringKey(999) != nullptr);
  CHECK(idx.findCoveringKey(999)->pts == 0);
  // The second keyframe's own extent is still open.
  CHECK(idx.findCoveringKey(1000) == nullptr);
}

TEST_CASE("KeyframeIndex: a reposition breaks the chain, so no extent is invented across it",
          "[keyframe_index]") {
  KeyframeIndex idx;
  idx.recordKey(0, 0, 100, scanned);
  idx.resetContiguity();  // a seek: the packets between the two were never read
  idx.recordKey(2000, 2000, 900, scanned);
  // There may be any number of keyframes in between; concluding that the first GOP runs to 2000
  // would send a later request to a keyframe that does not cover it.
  CHECK(idx.findCoveringKey(0) == nullptr);
  CHECK(idx.findCoveringKey(1000) == nullptr);
  CHECK(idx.findEntry(0) != nullptr);
  CHECK(idx.findEntry(2000) != nullptr);
}

TEST_CASE("KeyframeIndex: recording out of order keeps earlier extents intact",
          "[keyframe_index]") {
  KeyframeIndex idx;
  idx.recordKey(1000, 1000, 10, scanned);
  idx.recordKey(2000, 2000, 20, scanned);  // bounds the 1000 GOP
  REQUIRE(idx.findCoveringKey(1500) != nullptr);
  CHECK(idx.findCoveringKey(1500)->pts == 1000);

  // A keyframe from earlier in the stream: a later request seeks backwards, which resets the
  // contiguity chain, and the scan there records what it reads. It is inserted before both.
  idx.resetContiguity();
  idx.recordKey(500, 500, 5, scanned);
  CHECK(idx.findEntry(500) != nullptr);
  CHECK(idx.findCoveringKey(700) == nullptr);  // the 500 GOP is unbounded so far
  // What was already established is untouched by the insertion in front of it.
  REQUIRE(idx.findCoveringKey(1500) != nullptr);
  CHECK(idx.findCoveringKey(1500)->pts == 1000);

  // Scanning on from 500 reaches 1000 next, and that is the only keyframe that may bound it.
  idx.recordKey(1000, 1000, 10, scanned);
  REQUIRE(idx.findCoveringKey(700) != nullptr);
  CHECK(idx.findCoveringKey(700)->pts == 500);
  CHECK(idx.getGopHint() == 1000);  // 500 -> 1000 is shorter than the 1000 already known
}

TEST_CASE("KeyframeIndex: a keyframe read a second time is updated, not duplicated",
          "[keyframe_index]") {
  KeyframeIndex idx;
  idx.recordKey(1000, 1000, 10, scanned);
  idx.recordKey(2000, 2000, 20, scanned);
  // Read again after a byte seek: the byte position is what the entry is for, so the fresh one
  // wins, and the GOP extent already established survives.
  idx.recordKey(1000, 1000, 111, scanned);
  REQUIRE(idx.findEntry(1000) != nullptr);
  CHECK(idx.findEntry(1000)->pos == 111);
  // Exactly that keyframe, not the one before it: the byte-seek rewind seeks to whatever this
  // returns, so answering with a neighbour would put the demuxer in the middle of a GOP.
  CHECK(idx.findEntry(1500) == nullptr);
  CHECK(idx.findEntry(999) == nullptr);
  REQUIRE(idx.findCoveringKey(1500) != nullptr);
  CHECK(idx.findCoveringKey(1500)->pts == 1000);
}

TEST_CASE("KeyframeIndex: the GOP estimate is a lower bound that only grows", "[keyframe_index]") {
  KeyframeIndex idx;
  CHECK(idx.getGopHint() == 0);
  idx.recordKey(0, 0, 1, scanned);
  idx.recordKey(1000, 1000, 2, scanned);
  CHECK(idx.getGopHint() == 1000);
  idx.recordKey(1500, 1500, 3, scanned);  // a shorter GOP proves nothing about the longest
  CHECK(idx.getGopHint() == 1000);

  // The selection loop measures the same span from decoded frames instead of packets; the estimate
  // does not distinguish the two, because both are the same lower bound.
  idx.noteKeyframeSpan(0, 5000);
  CHECK(idx.getGopHint() == 5000);
  idx.noteKeyframeSpan(0, 100);
  CHECK(idx.getGopHint() == 5000);
  idx.noteKeyframeSpan(k::no_pts, 99999);  // nothing was observed: not a span
  CHECK(idx.getGopHint() == 5000);
  idx.noteKeyframeSpan(9000, 8000);  // backwards (a timestamp discontinuity)
  CHECK(idx.getGopHint() == 5000);
}

TEST_CASE("KeyframeIndex: the reorder delay is the smallest a keyframe has shown",
          "[keyframe_index]") {
  KeyframeIndex idx;
  // Nothing seen yet: the index is in the presentation domain and a query needs no shift.
  CHECK(idx.getReorderTicks() == 0);
  CHECK(idx.getIndexShift() == 0);
  CHECK(idx.pickIndexTs(/*dts=*/7, /*pts=*/9) == 9);

  const AVPacket a = key_packet(140, 100, 0);
  idx.notePacket(a, scanned);
  CHECK(idx.getReorderTicks() == 40);
  CHECK(idx.getIndexShift() == 40);
  // Keyframe packets carry a DTS, so the container's index is in the decode domain.
  CHECK(idx.pickIndexTs(/*dts=*/7, /*pts=*/9) == 7);

  // An open-GOP CRA with leading pictures shows a larger delay; the smallest is the real one, and
  // taking the largest would place every keyframe late.
  const AVPacket b = key_packet(1040, 1020, 10);
  idx.notePacket(b, scanned);
  CHECK(idx.getReorderTicks() == 20);
  const AVPacket c = key_packet(2100, 2000, 20);
  idx.notePacket(c, scanned);
  CHECK(idx.getReorderTicks() == 20);
}

TEST_CASE("KeyframeIndex: packets that say nothing about keyframes are ignored",
          "[keyframe_index]") {
  KeyframeIndex idx;
  AVPacket nonkey{};
  nonkey.pts = 500;
  nonkey.dts = 400;
  nonkey.pos = 42;
  idx.notePacket(nonkey, scanned);
  CHECK(idx.getReorderTicks() == 0);
  CHECK(idx.findEntry(500) == nullptr);

  // A keyframe whose DTS is ahead of its PTS is not a reorder delay; it is a broken stream.
  const AVPacket backwards = key_packet(100, 140, 0);
  idx.notePacket(backwards, scanned);
  CHECK(idx.getReorderTicks() == 0);
  CHECK(idx.getIndexShift() == 0);
  CHECK(idx.findEntry(100) != nullptr);  // still a keyframe, still worth its byte position
}

TEST_CASE("KeyframeIndex: a keyframe with no timestamp or no byte position is not recorded",
          "[keyframe_index]") {
  KeyframeIndex idx;
  idx.recordKey(k::no_pts, 0, 100, scanned);  // nothing to key it by
  idx.recordKey(1000, 1000, -1, scanned);     // nothing to seek back to
  CHECK(idx.findEntry(k::no_pts) == nullptr);
  CHECK(idx.findEntry(1000) == nullptr);
}

TEST_CASE("KeyframeIndex: a trusted container index is not shadowed by a recorded one",
          "[keyframe_index]") {
  KeyframeIndex idx;
  const AVPacket a = key_packet(140, 100, 0);
  idx.notePacket(a, indexed);
  // No entry: libavformat's index already answers where the keyframes are, and a second, partial
  // copy of it could only disagree.
  CHECK(idx.findEntry(140) == nullptr);
  // The reorder delay is learned anyway — it is what converts between the container index's decode
  // domain and presentation time, so it matters most exactly when the index is trusted.
  CHECK(idx.getReorderTicks() == 40);
  CHECK(idx.getIndexShift() == 40);
}

TEST_CASE("KeyframeIndex: a stream that ends inside a GOP bounds it at infinity",
          "[keyframe_index]") {
  KeyframeIndex idx;
  idx.recordKey(1000, 1000, 10, scanned);
  CHECK(idx.findCoveringKey(1000) == nullptr);  // extent unknown
  idx.noteStreamEndedInGop(1000);
  REQUIRE(idx.findCoveringKey(1000) != nullptr);
  CHECK(idx.findCoveringKey(1000)->pts == 1000);
  // Nothing follows it, so it answers for everything after it.
  REQUIRE(idx.findCoveringKey(std::numeric_limits<std::int64_t>::max() - 1) != nullptr);
  CHECK(idx.findCoveringKey(std::numeric_limits<std::int64_t>::max() - 1)->pts == 1000);

  idx.noteStreamEndedInGop(999999);  // never recorded: a no-op, not a new entry
  CHECK(idx.findEntry(999999) == nullptr);
}

TEST_CASE("KeyframeIndex: queries before the first keyframe find nothing", "[keyframe_index]") {
  KeyframeIndex empty;
  CHECK(empty.findCoveringKey(0) == nullptr);
  CHECK(empty.findEntry(0) == nullptr);

  KeyframeIndex idx;
  idx.recordKey(1000, 1000, 10, scanned);
  idx.recordKey(2000, 2000, 20, scanned);
  CHECK(idx.findCoveringKey(999) == nullptr);
}

TEST_CASE("KeyframeIndex: doesKeyCover and hasKeyBetween without a container index",
          "[keyframe_index]") {
  KeyframeIndex idx;
  idx.recordKey(1000, 1000, 10, scanned);
  idx.recordKey(2000, 2000, 20, scanned);

  CHECK(idx.doesKeyCover(1000, 1500, scanned));
  CHECK_FALSE(idx.doesKeyCover(2000, 1500, scanned));  // a keyframe after P covers nothing
  CHECK_FALSE(idx.doesKeyCover(1000, 2500, scanned));  // the 2000 GOP is unbounded, so unknown
  CHECK_FALSE(idx.doesKeyCover(500, 1500, scanned));   // not the covering keyframe

  // Without a container index there is nothing to contradict the recorded one, so the scan is
  // never sent on by this.
  CHECK_FALSE(idx.hasKeyBetween(1000, 1500, scanned));
  CHECK_FALSE(idx.hasKeyBetween(0, std::numeric_limits<std::int64_t>::max(), scanned));
}

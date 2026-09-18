#pragma once
// stills/detail/stills_FrameSelector.h — the decode loop and the choice of frame: which decoded frame
// answers a request, and everything that has to be remembered to make that choice.
//
// FrameSelector owns the three frame slots (the held frame, the one frame of look-ahead that proves
// the held frame covers the request, and the last frame the decoder flagged corrupt), the
// DecodeFrontier, the per-request selection mode and the end-of-stream policy. It pulls frames out
// of the VideoDecoder, feeds it packets from the PacketReader, and decides when a frame is the
// answer. It never seeks and never re-opens: when the loop finds that the demuxer is in the wrong
// place it stops and says so (Step::Action), and FramePipeline performs the reposition and calls
// select() again. That is the same one-way arrangement Positioner and PacketReader::readLanding()
// use, for the same reason: a seek invalidates every other type's state, which only the owner of
// all of them may do.
//
// Everything a step reads from the pipeline's other parts arrives as a SelectionContext, built at
// the call and never stored (a re-open replaces the AVFormatContext and AVStream underneath the
// source). The one thing the selector keeps between calls is its own state: the slots, the
// frontier, the mode of the request in progress and the counters.
//
// The two per-request fields that have to agree — `keyframeOnly` and `skipBeforeTs` — are written
// together by beginRequest() and nowhere else, so nearest-keyframe mode cannot be entered with a
// skip window armed.
//
// Timestamps are in the stream's own time base throughout.
//
// Not thread-safe: a FramePipeline is single-threaded by contract (stills_FramePipeline.h), and its
// FrameSelector is only ever touched by the thread running that pipeline's decode loop.

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "stills/detail/stills_DecodeFrontier.h"
#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_FrameSlot.h"
#include "stills/detail/stills_KeyframeIndex.h"
#include "stills/detail/stills_MediaSource.h"
#include "stills/detail/stills_PacketReader.h"
#include "stills/detail/stills_Positioner.h"
#include "stills/detail/stills_SeekCostModel.h"
#include "stills/detail/stills_VideoDecoder.h"
#include "stills/stills_Error.h"
#include "stills/stills_Image.h"
#include "stills/stills_Options.h"
#include "stills/stills_Time.h"

namespace stills::detail
{

// What a selection step reads and drives, gathered at the call site. Built per call and never
// stored. The mutable references are the objects the loop has to act on (pull a frame, read a
// packet, record a keyframe, note that one was fed); the values are request facts the pipeline
// owns.
struct SelectionContext
{
    MediaSource& source;
    VideoDecoder& decoder;
    PacketReader& packets;
    KeyframeIndex& keys;
    Position& position;
    SeekCostModel& costs;
    const Positioner& positioner;
    std::int64_t firstFrameTs; // presentation time of the stream's first frame; k::noPts until probed
    OutOfRangePolicy outOfRange;
};

class FrameSelector
{
public:
    // A frame chosen for a request, owned by the selector (one of its FrameSlots). Valid until the
    // next call that may move a frame: a reposition, a promotion or another select().
    struct Selected
    {
        AVFrame* frame{ nullptr };
        Adjustment adjustment{ Adjustment::none };
        bool corrupt{ false };
    };

    // One pass of the selection loop. `done` carries the answer or the failure; the other two ask
    // the caller to reposition and call select() again, because the loop found the demuxer is not
    // where the request needs it and moving it is not the selector's to do.
    struct Step
    {
        enum class Action
        {
            done,
            backOff,            // the landing proved to be past the request: retreat, then continue
            retryAfterEmptyEof, // an end of stream that produced no frame at all: clear it and re-position
        };

        Action action{ Action::done };
        std::expected<Selected, Error> result{ Selected{} }; // meaningful for `done` only
        std::int64_t observedKey{ k::noPts };                // `backOff`: the keyframe seen past the request, if any
    };

    FrameSelector() = default;
    FrameSelector (const FrameSelector&) = delete;
    FrameSelector& operator= (const FrameSelector&) = delete;

    // Allocates the three slots' frames. Runs after every MediaSource open and re-open; the slots
    // start empty.
    [[nodiscard]] std::expected<void, Error> attach()
    {
        for (FrameSlot* slot : { &held, &pending, &corruptLast })
        {
            auto f = makeFrame();

            if (! f) return std::unexpected (f.error());
            slot->install (std::move (*f));
        }

        return {};
    }

    // Forgets everything concluded since the last reposition: the three frames, the frontier
    // (holes included) and the run of consecutive decode errors. The caller drops the decoder's own
    // received frame and the reader's position alongside; the recorded keyframes survive.
    void reset() noexcept
    {
        held.clear();
        pending.clear();
        corruptLast.clear();
        frontier.reset();
        decodeErrors = 0;
    }

    // Where synthesised timestamps resume after a seek: the target. Set after reset(), which clears
    // it.
    void setSynthOrigin (std::int64_t target) noexcept { frontier.synthTs = target; }

    // The one field a recovery may carry across a reset(): on a source that cannot be rewound, the
    // demuxer really is still at or after the last frame that came out. Read before the reset,
    // written back after it, by FramePipeline::recoverAfterInterrupt() and nothing else.
    [[nodiscard]] std::int64_t getLastReceivedTs() const noexcept { return frontier.lastReceivedTs; }
    void carryForwardLastReceivedTs (std::int64_t ts) noexcept { frontier.lastReceivedTs = ts; }

    // Read-only view for the positioning decisions (PositioningView).
    [[nodiscard]] const DecodeFrontier& getFrontier() const noexcept { return frontier; }

    // A demux error forced packets to be skipped during a landing read the pipeline drove: there is
    // a hole in what the decoder will be fed, and every frame until the next keyframe is suspect.
    void noteDemuxTaint() noexcept { frontier.tainted = true; }

    // A re-opened (or grown) source may reach further than the old one did.
    void resetTailEnd() noexcept { tailEnd = k::noPts; }

    // One re-position per attempt after an end of stream that decoded nothing; a hardware fallback
    // retries the request and gets its own.
    void beginAttempt() noexcept { eofRetried = false; }

    // Arms the mode of the request about to be selected. Nearest-keyframe mode never skips frames
    // (the decoder must produce the keyframe itself); exact mode may skip non-reference frames that
    // provably end before the tolerance window, but only on a demuxer whose keyframe flags can be
    // trusted. The two fields are written here together so they cannot disagree.
    void beginRequest (bool keyframeMode, std::int64_t lo, const SelectionContext& ctx) noexcept
    {
        keyframeOnly = keyframeMode;
        skipBeforeTs = k::noPts;

        if (! keyframeMode && ctx.packets.areKeyFlagsReliable())
        {
            const std::int64_t margin =
                ctx.keys.getReorderTicks() + Positioner::framesTicks (ctx.source.getStreamInfo(), 2);
            skipBeforeTs = lo > std::numeric_limits<std::int64_t>::min() + margin ? lo - margin : lo;
        }
    }

    // The open-time probe decodes the first frame in exact mode. It leaves the skip window as it
    // was: a probe run inside a request (a decoder rebuild) decodes under that request's window.
    void beginProbe() noexcept { keyframeOnly = false; }

    // Nearest-keyframe mode found its keyframe precedes the first presented frame: the request is
    // answered exactly instead. The skip window stays unarmed, as keyframe mode left it.
    void useExactMode() noexcept { keyframeOnly = false; }

    [[nodiscard]] bool isKeyframeOnly() const noexcept { return keyframeOnly; }
    // Whether the request in progress armed a skip window. A request abandoned with one armed may
    // have left frames ahead of the decoder that will never be produced.
    [[nodiscard]] bool hasSkipWindow() const noexcept { return skipBeforeTs != k::noPts; }

    [[nodiscard]] std::int64_t getFrameTs (const AVFrame& f, const StreamInfo& stream) const noexcept
    {
        if (f.best_effort_timestamp != k::noPts) return f.best_effort_timestamp;
        if (f.pts != k::noPts) return f.pts;
        if (f.pkt_dts != k::noPts) return f.pkt_dts;
        return frontier.synthTs != k::noPts ? frontier.synthTs : stream.startPts;
    }

    [[nodiscard]] static std::int64_t getFrameDuration (const AVFrame& f, const StreamInfo& stream) noexcept
    {
        return f.duration > 0 ? f.duration : stream.frameDurationHint;
    }

    // ---- answering without repositioning -----------------------------------------------------

    // Fast path: the frame covering `P` is already held and nothing later covers it instead. Empty
    // when the request has to position. In nearest-keyframe mode the held frame must be the
    // keyframe the index says covers `P`; in exact mode the look-ahead frame or the end of stream
    // must rule out a later frame.
    //
    // Runs before beginRequest(): the end-of-stream answer is formed under the previous request's
    // mode, as it always was.
    [[nodiscard]] std::optional<std::expected<Selected, Error>> answerFromHeld (std::int64_t P, bool keyframeMode,
                                                                                Adjustment clampedByBounds,
                                                                                const SelectionContext& ctx,
                                                                                const CancelToken& token)
    {
        const StreamInfo& stream = ctx.source.getStreamInfo();

        if (! isHeldFrameCovering (P, stream)) return std::nullopt;
        if (keyframeMode)
        {
            // The held frame is the keyframe covering P (the index says no keyframe lies in (h, P]).
            const std::int64_t h = getFrameTs (*held.getFrame(), stream);

            if ((held.getFrame()->flags & AV_FRAME_FLAG_KEY) != 0
                && ctx.keys.doesKeyCover (h, P, containerIndexOf (ctx.source)))
            {
                return verifyKeyframeTail (Selected{ held.getFrame(), clampedByBounds, held.isConcealed() }, P, ctx,
                                           token);
            }

            return std::nullopt;
        }

        if (pending.isValid() && P < getFrameTs (*pending.getFrame(), stream))
        {
            return Selected{ held.getFrame(), clampedByBounds, held.isConcealed() };
        }

        if (frontier.eof && ! pending.isValid() && isHeldFrameAtTail (stream))
        {
            auto s = finishAtEof (P, ctx);

            if (! s) return std::unexpected (std::move (s.error()));
            if (s->adjustment == Adjustment::none) s->adjustment = clampedByBounds;
            return *s;
        }

        return std::nullopt;
    }

    // Whether the look-ahead frame can become the held frame and the decode continue from there.
    //
    // The invariant: the pending frame begins at or before `target`, so the frame on screen at
    // `target` is it or one the decoder has yet to produce; every frame between it and `target` will
    // be produced; and reaching `target` that way costs less than seeking to it.
    [[nodiscard]] bool canPromotePendingFrame (std::int64_t target, const SelectionContext& ctx) const noexcept
    {
        if (! pending.isValid() || frontier.drained) return false; // isValid() guards getFrame()
        const std::int64_t p = getFrameTs (*pending.getFrame(), ctx.source.getStreamInfo());
        return target >= p && ! frontier.skipped.containsIn (p, target)
               && ctx.positioner.isForwardCheaperThanSeek (target, positioningView (ctx));
    }

    // Makes the look-ahead frame the held frame. Precondition: canPromotePendingFrame() held.
    void promotePending() noexcept { held.adopt (pending); }

    // Whether the request can be answered without repositioning, by decoding on from the held frame.
    //
    // The invariant: the decoder's next output is the frame after the held one, and every frame
    // between the held frame and `target` will be produced. An empty look-ahead slot is what says
    // nothing past the held frame has been received; `eof` and `drained` say the decoder has no more
    // to give without a flush; a recorded hole says one of the frames in between was skipped and
    // never will be produced. Any of those failing leaves the frame on screen at `target`
    // unestablishable from here, and the request has to position.
    [[nodiscard]] bool canContinueFromHeldFrame (std::int64_t target, const SelectionContext& ctx) const noexcept
    {
        if (! held.isValid() || pending.isValid() || frontier.eof || frontier.drained)
        {
            return false; // isValid() guards getFrame()
        }

        const std::int64_t h = getFrameTs (*held.getFrame(), ctx.source.getStreamInfo());
        return h <= target && ! frontier.skipped.containsIn (h, target)
               && ctx.positioner.isForwardCheaperThanSeek (target, positioningView (ctx));
    }

    // ---- nearest-keyframe mode ---------------------------------------------------------------

    // Whether the keyframe packet positioning left pending precedes the first presented frame: an
    // edit list trimmed its GOP, so it cannot be the answer.
    [[nodiscard]] bool isPreEditKeyframe (const SelectionContext& ctx) const noexcept
    {
        if (! ctx.packets.isPacketPending()) return false;
        const AVPacket& pkt = ctx.packets.getPacket();
        return (pkt.flags & AV_PKT_FLAG_DISCARD) != 0
               || (ctx.firstFrameTs != k::noPts && pkt.pts != k::noPts && pkt.pts < ctx.firstFrameTs);
    }

    // Feeds the pending keyframe packet and drains, so the decoder emits that one frame at once
    // instead of after a pipeline's worth of packets (frame threads hold ~thread_count packets).
    // The decoder must be flushed before it is fed again (`frontier.drained`). A packet that cannot
    // be singled out as a keyframe is left to the normal decode.
    [[nodiscard]] std::expected<void, Error> feedKeyframeAndDrain (const SelectionContext& ctx) noexcept
    {
        if (! ctx.packets.areKeyFlagsReliable() || ctx.packets.getPacket().pts == k::noPts)
            return {}; // cannot single out a keyframe: decode normally
        ctx.position.noteKeyframeFed();
        const int s = sendHeldPacket (ctx);

        if (s < 0 && s != k::eagain) return fail (ErrorCode::decodeFailed, s, "avcodec_send_packet (keyframe)");
        ctx.decoder.startDrain();
        frontier.draining = true;
        frontier.drained = true;
        frontier.landingKnown = true; // this keyframe is the answer; select() need not prove it again
        return {};
    }

    // Nearest-keyframe mode answers with the keyframe at or before P without decoding past it, so it
    // cannot tell a long GOP from the tail of a truncated file: a request past the end would come
    // back as the last keyframe, unflagged, where exact mode reports timeOutOfRange. When the
    // chosen keyframe is more than a GOP behind the request, read ahead (packets only) until one
    // proves the stream reaches P or the source ends. Remembered in tailEnd: once per source.
    [[nodiscard]] std::expected<Selected, Error>
    verifyKeyframeTail (Selected sel, std::int64_t P, const SelectionContext& ctx, const CancelToken& token)
    {
        const StreamInfo& stream = ctx.source.getStreamInfo();

        if (sel.frame == nullptr) return sel;
        const std::int64_t t = getFrameTs (*sel.frame, stream);
        const std::int64_t gop =
            std::max ({ ctx.keys.getGopHint(), Positioner::framesTicks (stream, 2), std::int64_t{ 1 } });

        if (P <= t + gop || P <= ctx.packets.getVerifiedTo()) return sel; // plainly inside the data
        if (tailEnd == k::noPts)
        {
            // The demuxer sits just after the chosen keyframe: read on until the question is answered.
            for (;;)
            {
                const int r = ctx.packets.readVideoPacket (ctx.source, ctx.keys, frontier.tainted, token);

                if (r == k::exitRequested)
                {
                    if (token.isRequested()) return fail (ErrorCode::cancelled, "cancelled");
                    return sel; // an I/O hiccup is not proof of anything: keep the keyframe
                }

                if (r == k::eof)
                {
                    const std::int64_t verified = ctx.packets.getVerifiedTo();
                    const std::int64_t oneFrame = std::max<std::int64_t> (stream.frameDurationHint, 1);
                    tailEnd = (verified != k::noPts ? verified : t) + oneFrame;
                    break;
                }

                ctx.packets.unrefPacket();
                const std::int64_t verified = ctx.packets.getVerifiedTo();

                if (verified != k::noPts && verified > P) break;
            }

            ctx.position.markInvalid(); // the demuxer has moved; the next request repositions
        }

        if (tailEnd == k::noPts || P < tailEnd) return sel;
        if (ctx.outOfRange == OutOfRangePolicy::clampToLastFrame)
        {
            sel.adjustment = Adjustment::clampedToLast;
            return sel;
        }

        return fail (ErrorCode::timeOutOfRange,
                     "requested time is past the last frame ("
                         + toString (Time::fromTimestamp (std::max<std::int64_t> (tailEnd - stream.startPts, 0),
                                                          fromAv (stream.timeBase)))
                         + ")");
    }

    // ---- the selection loop ------------------------------------------------------------------

    // Core selection loop. Precondition: positioned (or continuing forward). Runs until a frame
    // answers `P` within `[lo, hi]`, the request fails, or the demuxer has to be moved — in which
    // case the step says how (Step::Action) and the caller comes back here once it has.
    [[nodiscard]] Step select (std::int64_t P, std::int64_t lo, std::int64_t hi, const SelectionContext& ctx,
                               const CancelToken& token)
    {
        const StreamInfo& stream = ctx.source.getStreamInfo();

        for (;;)
        {
            if (token.isRequested()) return finished (fail (ErrorCode::cancelled, "cancelled"));
            const int r = receiveOne (ctx.decoder);

            if (r == 0)
            {
                decodeErrors = 0;
                ctx.costs.noteFirstFrame();

                if (stream.synthesizeTimestamps)
                {
                    const std::int64_t stamped = frontier.synthTs != k::noPts ? frontier.synthTs : stream.startPts;
                    AVFrame& got = ctx.decoder.getFrame();
                    got.pts = got.best_effort_timestamp = stamped;
                    got.duration = stream.frameDurationHint;
                }

                const AVFrame& got = ctx.decoder.getFrame();
                const std::int64_t t = getFrameTs (got, stream);
                frontier.synthTs = t + getFrameDuration (got, stream);
                frontier.lastReceivedTs = t;
                ++frontier.receivedSinceSeek;
                const bool key = (got.flags & AV_FRAME_FLAG_KEY) != 0;
                // GOP length estimate (a lower bound, exact at the next keyframe): feeds the back-off step
                // and the forward-scan limit.
                ctx.keys.noteKeyframeSpan (frontier.lastKeyTs, t);

                if (key) frontier.lastKeyTs = t;
                if ((got.flags & AV_FRAME_FLAG_CORRUPT) != 0)
                {
                    frontier.skipped.record (t); // not produced: nothing may decode across it
                    // Concealed by construction: this is the frame the decoder itself flagged, stashed only
                    // as a fallback for a stream that never produces a better one (finishAtEof).
                    corruptLast.adopt (ctx.decoder.getFrame(), /*wasConcealed=*/true);
                    continue;
                }

                // Corrupt: the decoder reported concealment, or a decode error occurred since the last
                // keyframe (the reference chain is suspect).
                if (key) frontier.tainted = false;
                const bool concealed = got.decode_error_flags != 0 || frontier.tainted;
                frontier.lastEnd = std::max (frontier.lastEnd, t + getFrameDuration (got, stream));
                frontier.skipped.clearAt (t); // whatever was skipped here has now been produced

                if (keyframeOnly)
                {
                    // "The keyframe at or before P": the fed keyframe after a packet-level landing
                    // (frontier.landingKnown), or the first keyframe out after a seek aimed at P itself.
                    // After a back-off (the seek target is below P) that guarantee is gone: keep decoding,
                    // remembering the last keyframe <= P, until a frame past P shows up.
                    if (t <= P)
                    {
                        if (key || ! held.isValid())
                            holdReceived (ctx.decoder, concealed); // a non-key frame only as a fallback
                        if (key
                            && (frontier.landingKnown || t == P
                                || (frontier.receivedSinceSeek == 1 && ctx.position.getSeekTarget() == P)))
                        {
                            return finished (heldAsAnswer());
                        }

                        continue;
                    }

                    if (held.isValid())
                    {
                        pendReceived (ctx.decoder, concealed);
                        return finished (heldAsAnswer());
                    }
                }
                else
                {
                    if (t < P)
                    {
                        holdReceived (ctx.decoder, concealed);

                        if (t >= lo) return finished (heldAsAnswer()); // early accept within tolerance
                        continue;
                    }

                    if (t == P)
                    {
                        holdReceived (ctx.decoder, concealed);
                        return finished (heldAsAnswer());
                    }

                    if (held.isValid())
                    {
                        pendReceived (ctx.decoder, concealed);
                        return finished (heldAsAnswer());
                    }
                }

                // Overshoot: the first frame out is already past P. Either P precedes the first frame of
                // the stream (provable only after the explicit start seek) or the seek landed late. A late
                // landing is backed off even when a later frame would satisfy the `after` tolerance: the
                // frame actually on screen at P exists until proven otherwise.
                const bool atStart = ctx.position.isLandedAtStart() || ! stream.seekable
                                     || (ctx.firstFrameTs != k::noPts && t <= ctx.firstFrameTs);
                const bool landing = frontier.receivedSinceSeek == 1;
                const std::int64_t observedKey = key ? t : k::noPts;

                if (landing && ! atStart) return backOff (observedKey);
                if (t <= hi)
                {
                    holdReceived (ctx.decoder, concealed);
                    return finished (heldAsAnswer());
                }

                if (atStart)
                {
                    holdReceived (ctx.decoder, concealed);
                    return finished (Selected{ held.getFrame(), Adjustment::clampedToFirst, held.isConcealed() });
                }

                return backOff (observedKey);
            }

            if (r == k::eof)
            {
                // Nothing decodable between the landing and the end of the stream. Back off unless the
                // start seek has already been done, in which case the stream really has no frame for P.
                if (! held.isValid() && ! corruptLast.isValid() && stream.seekable && ctx.position.isPositioned()
                    && ! ctx.position.isLandedAtStart() && ! frontier.drained)
                {
                    return backOff (k::noPts);
                }

                // Not one frame came out of the decoder since this request positioned. On a seekable
                // source that says nothing about the stream: the start seek landed and the very first read
                // reported the end, which is what libavio's sticky eof_reached does after a cancellation
                // (a seek does not clear it). Clear it and re-position once before concluding the stream
                // ended — the landed-at-start test above would otherwise accept that first read as proof.
                if (! held.isValid() && ! corruptLast.isValid() && stream.seekable && ! frontier.drained
                    && frontier.receivedSinceSeek == 0 && ! eofRetried)
                {
                    eofRetried = true;
                    return Step{ Step::Action::retryAfterEmptyEof, Selected{}, k::noPts };
                }

                frontier.eof = true;

                if (frontier.lastEnd != std::numeric_limits<std::int64_t>::min())
                {
                    tailEnd = tailEnd == k::noPts ? frontier.lastEnd : std::max (tailEnd, frontier.lastEnd);
                }

                return finished (finishAtEof (P, ctx));
            }

            if (r == k::eagain)
            {
                const int f = feedOne (ctx, token);

                if (f == 0 || f == k::eof) continue;
                if (f == k::exitRequested)
                {
                    if (token.isRequested()) return finished (fail (ErrorCode::cancelled, "cancelled"));
                    return finished (
                        fail (ErrorCode::decodeFailed, f, "av_read_frame: interrupted without a cancellation"));
                }

                if (ctx.decoder.isHardwareFault (f))
                {
                    ctx.decoder.noteHardwareFault();
                    return finished (fail (ErrorCode::decodeFailed, f, "avcodec_send_packet (hardware)"));
                }

                return finished (fail (ErrorCode::decodeFailed, f, "avcodec_send_packet"));
            }

            ++totalDecodeErrors;
            frontier.tainted = true;

            if (ctx.decoder.isHardwareFault (r))
            {
                ctx.decoder.noteHardwareFault();
                return finished (fail (ErrorCode::decodeFailed, r, "avcodec_receive_frame (hardware)"));
            }

            if (++decodeErrors > maxConsecutiveErrors)
            {
                return finished (
                    fail (ErrorCode::decodeFailed, r, "avcodec_receive_frame: too many consecutive errors"));
            }
        }
    }

private:
    static constexpr int maxConsecutiveErrors = 32;

    [[nodiscard]] static Step finished (std::expected<Selected, Error> result) noexcept
    {
        return Step{ Step::Action::done, std::move (result), k::noPts };
    }

    [[nodiscard]] static Step backOff (std::int64_t observedKey) noexcept
    {
        return Step{ Step::Action::backOff, Selected{}, observedKey };
    }

    [[nodiscard]] Selected heldAsAnswer() const noexcept
    {
        return Selected{ held.getFrame(), Adjustment::none, held.isConcealed() };
    }

    [[nodiscard]] PositioningView positioningView (const SelectionContext& ctx) const noexcept
    {
        return PositioningView{ ctx.source.getStreamInfo(),   ctx.keys, frontier, ctx.costs, ctx.position,
                                containerIndexOf (ctx.source) };
    }

    // The received frame becomes the answer. The look-ahead slot is emptied with it: no frame past
    // the new held one has been seen yet, and a stale one there would claim it covers the request.
    void holdReceived (VideoDecoder& decoder, bool concealed) noexcept
    {
        held.adopt (decoder.getFrame(), concealed);
        pending.clear();
    }

    // One frame of look-ahead past the held frame -- what proves the held frame covers the request.
    void pendReceived (VideoDecoder& decoder, bool concealed) noexcept
    {
        pending.adopt (decoder.getFrame(), concealed);
    }

    // Pulls one frame into the decoder's frame. Returns 0, k::eof, k::eagain (needs a packet) or
    // an error.
    //
    // The one thing added to VideoDecoder::receive(): once the drain has started, "nothing yet" is
    // the end of the stream rather than a request for another packet. Whether a drain is in
    // progress is the decode loop's own state, so the translation is made here and not inside the
    // decoder.
    [[nodiscard]] int receiveOne (VideoDecoder& decoder) noexcept
    {
        const int r = decoder.receive();

        if (r == k::eagain && frontier.draining) return k::eof;
        return r;
    }

    // Sends the reader's live packet. Returns 0 (consumed), k::eagain (kept for a retry after the
    // decoder has been drained) or an error (packet dropped).
    [[nodiscard]] int sendHeldPacket (const SelectionContext& ctx) noexcept
    {
        AVPacket& pkt = ctx.packets.getPacket();
        const std::int64_t fedDts = pkt.dts;
        const std::int64_t fedPts = pkt.pts;
        const int s = ctx.decoder.send (pkt);

        if (s == k::eagain)
        {
            ctx.packets.holdPacketForRetry(); // the decoder wants a receive first; keep the packet
            return s;
        }

        if (s == 0)
        {
            if (fedDts != k::noPts) frontier.lastFedDts = fedDts;
            if (fedPts != k::noPts)
                frontier.lastFedPts = frontier.lastFedPts == k::noPts ? fedPts : std::max (frontier.lastFedPts, fedPts);
        }

        ctx.packets.releasePacket();
        return s;
    }

    // Reads the next packet of our stream and feeds it. Returns 0, k::eof (drain started),
    // k::exitRequested (interrupted) or a decoder error.
    // Not noexcept: the skipped-frame record below it allocates.
    [[nodiscard]] int feedOne (const SelectionContext& ctx, const CancelToken& token)
    {
        if (ctx.packets.isPacketPending())
        {
            if ((ctx.packets.getPacket().flags & AV_PKT_FLAG_KEY) != 0) ctx.position.noteKeyframeFed();
            prepareSend (ctx);
            const int s = sendHeldPacket (ctx);

            if (s == k::eagain)
            {
                // Both receive and send report EAGAIN: the decoder violates its contract.
                ctx.packets.releasePacket();
                return k::einval;
            }

            if (s == 0) return 0;
            if (s != k::invalidData) return s;
            ++totalDecodeErrors;
            frontier.tainted = true;
        }

        if (frontier.drained) return k::eof; // the decoder was drained for a keyframe-only decode; a seek resets it
        for (;;)
        {
            const int r = ctx.packets.readVideoPacket (ctx.source, ctx.keys, frontier.tainted, token);

            if (r == k::exitRequested) return r;
            if (r == k::eof)
            {
                frontier.draining = true;
                ctx.decoder.startDrain();
                return k::eof;
            }

            const bool keyPacket = (ctx.packets.getPacket().flags & AV_PKT_FLAG_KEY) != 0;

            if (ctx.position.isAwaitingKey() && ctx.packets.areKeyFlagsReliable() && ! keyPacket)
            {
                // Mid-GOP after a seek: the decoder would decode these in full and drop them anyway.
                ctx.packets.unrefPacket();
                continue;
            }

            if (keyPacket) ctx.position.noteKeyframeFed();
            prepareSend (ctx);
            const int s = sendHeldPacket (ctx);

            if (s == 0 || s == k::eagain) return 0;
            if (s == k::invalidData)
            {
                ++totalDecodeErrors;
                frontier.tainted = true;

                if (++decodeErrors > maxConsecutiveErrors) return s;
                continue;
            }

            return s;
        }
    }

    // Before a packet goes to the decoder: non-reference frames far before the target are skipped
    // at the decoder level (AVDISCARD_NONREF).
    //
    // A frame may be skipped only when its display interval provably ends before the window.
    // Durations cannot prove that (mov stores decode-order deltas, so a VFR stream's frame may say
    // "1/30 s"), nor can the average frame rate. What does: a frame with a *later* presentation
    // time has already been fed (B-frames follow their forward reference in decode order) and that
    // later time is itself before the window.
    void prepareSend (const SelectionContext& ctx)
    {
        const AVPacket& pkt = ctx.packets.getPacket();
        const bool key = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
        AVDiscard skip = AVDISCARD_DEFAULT;

        if (skipBeforeTs != k::noPts && ctx.packets.areKeyFlagsReliable() && ! key && pkt.pts != k::noPts
            && frontier.lastFedPts != k::noPts)
        {
            const std::int64_t pts = pkt.pts;

            if (pts < frontier.lastFedPts && frontier.lastFedPts <= skipBeforeTs)
            {
                skip = AVDISCARD_NONREF;
                frontier.skipped.record (pts); // if the decoder drops it, nothing may decode across it
            }
        }

        ctx.decoder.setSkipPolicy (skip);
    }

    // The answer once the stream has ended: the held frame if `P` falls inside its display interval
    // (recomputed from the frame, never remembered as a flag), the stashed corrupt frame as a last
    // resort, otherwise the failure that says why nothing could be produced.
    [[nodiscard]] std::expected<Selected, Error> finishAtEof (std::int64_t P, const SelectionContext& ctx)
    {
        const StreamInfo& stream = ctx.source.getStreamInfo();
        const auto finish = [&] (AVFrame* f, bool corrupt) -> std::expected<Selected, Error>
        {
            const std::int64_t t = getFrameTs (*f, stream);
            // In keyframe mode the stream extends past the held keyframe to the last frame
            // decoded after it.
            const std::int64_t end = std::max (t + getFrameDuration (*f, stream), keyframeOnly ? frontier.lastEnd : t);

            if (P < end) return Selected{ f, Adjustment::none, corrupt };
            if (ctx.outOfRange == OutOfRangePolicy::clampToLastFrame)
                return Selected{ f, Adjustment::clampedToLast, corrupt };
            return fail (ErrorCode::timeOutOfRange,
                         "requested time is past the last frame ("
                             + toString (Time::fromTimestamp (std::max<std::int64_t> (t - stream.startPts, 0),
                                                              fromAv (stream.timeBase)))
                             + ")");
        };

        if (held.isValid()) return finish (held.getFrame(), held.isConcealed());
        if (corruptLast.isValid())
        {
            held.adopt (corruptLast); // carries the concealed flag the stash was adopted with

            // Nearest-keyframe mode asks for "the keyframe at or before P", and a stashed frame at or
            // before P answers that: its display interval is not the question, as it is in exact mode.
            if (keyframeOnly && P >= getFrameTs (*held.getFrame(), stream))
                return Selected{ held.getFrame(), Adjustment::none, true };
            return finish (held.getFrame(), true);
        }

        if (totalDecodeErrors > 0)
        {
            return fail (ErrorCode::decodeFailed,
                         "no decodable frame found (" + std::to_string (totalDecodeErrors) + " decode errors)");
        }

        return fail (ErrorCode::endOfStream, "the stream ended before a frame for the requested time was decoded");
    }

    // Whether the held frame really is the last frame in presentation order of everything decoded
    // since positioning. It is, for every monotonic stream. It is not on a container whose
    // timestamps jump backwards (two recordings concatenated into one MPEG-TS, a camera restart):
    // decoding forward across the jump ends with a frame from the *earlier* segment, and concluding
    // "the stream has no frame past this one" from it strands the generator, because the
    // end-of-stream shortcut then answers every later request without ever repositioning.
    [[nodiscard]] bool isHeldFrameAtTail (const StreamInfo& stream) const noexcept
    {
        if (! held.isValid()) return false;
        return getFrameTs (*held.getFrame(), stream) + getFrameDuration (*held.getFrame(), stream) >= frontier.lastEnd;
    }

    // Whether the held frame is the one on screen at `target`, as far as the frontier can say.
    //
    // The invariant: the held frame's display interval starts at or before `target` and every frame
    // between the two was produced. A frame skipped in between is the one on screen instead, so the
    // held frame does not cover `target` after all, whatever the look-ahead or the end of stream
    // say. What rules out a *later* frame covering `target` is mode-specific and stays with the
    // caller: the look-ahead frame or the end of stream in exact mode, the absence of a keyframe in
    // (held, target] in nearest-keyframe mode.
    [[nodiscard]] bool isHeldFrameCovering (std::int64_t target, const StreamInfo& stream) const noexcept
    {
        if (! held.isValid()) return false; // guards getFrame()
        const std::int64_t h = getFrameTs (*held.getFrame(), stream);
        return h <= target && ! frontier.skipped.containsIn (h, target);
    }

    FrameSlot held, pending, corruptLast;
    DecodeFrontier frontier; // where the decoder is; every reposition resets it
    // One re-position per request after an end of stream that decoded nothing (see select()).
    bool eofRetried{ false };
    bool keyframeOnly{ false };            // the current request is in nearest-keyframe mode
    std::int64_t skipBeforeTs{ k::noPts }; // packets whose frames end before this may skip non-reference frames
    std::int64_t tailEnd{ k::noPts };      // end of the data once the end of stream has been observed
                                           // (k::noPts = not yet)
    int decodeErrors{ 0 };
    long totalDecodeErrors{ 0 };
};

} // namespace stills::detail

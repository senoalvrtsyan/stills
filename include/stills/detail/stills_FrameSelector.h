#pragma once
// stills/detail/stills_FrameSelector.h — the decode loop and the choice of frame: which decoded frame
// answers a request, and everything that has to be remembered to make that choice.
//
// FrameSelector owns the three frame slots (the held frame, the one frame of look-ahead that proves
// the held frame covers the request, and the last frame the decoder flagged corrupt), the
// DecodeFrontier, the per-request selection mode and the end-of-stream policy. It never seeks and
// never re-opens: when the loop finds the demuxer in the wrong place it stops and says so
// (Step::Action), and the owner repositions and calls select() again.
//
// Everything a step reads from the pipeline's other parts arrives as a SelectionContext, built at
// the call and never stored, because a re-open replaces the AVFormatContext and AVStream underneath
// the source.
//
// `keyframeOnly` and `skipBeforeTs` are written together by beginRequest(); beginProbe() and
// useExactMode() only ever clear the mode, so a skip window can never be armed while the request is
// in nearest-keyframe mode.
//
// Timestamps are in the stream's own time base throughout. Not thread-safe: a FramePipeline is
// single-threaded by contract, and its FrameSelector is only ever touched by that thread.

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
#include "stills/detail/stills_RequestWindow.h"
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
    PacketReader& packetReader;
    KeyframeIndex& keyframes;
    Position& position;
    SeekCostModel& costs;
    const Positioner& positioner;
    std::int64_t firstFrameTs; // presentation time of the stream's first frame; libav::noPts until probed
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
        std::int64_t observedKey{ libav::noPts };            // `backOff`: the keyframe seen past the request, if any
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
            auto frame = makeFrame();

            if (! frame) return std::unexpected (frame.error());
            slot->install (std::move (*frame));
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
    // written back after it, by the interrupt recovery and nothing else.
    [[nodiscard]] std::int64_t getLastReceivedTs() const noexcept { return frontier.lastReceivedTs; }
    void carryForwardLastReceivedTs (std::int64_t ts) noexcept { frontier.lastReceivedTs = ts; }

    // Read-only view for the positioning decisions (PositioningView).
    [[nodiscard]] const DecodeFrontier& getFrontier() const noexcept { return frontier; }

    // A demux error forced packets to be skipped during a landing read the pipeline drove: there is
    // a hole in what the decoder will be fed, and every frame until the next keyframe is suspect.
    void noteDemuxTaint() noexcept { frontier.tainted = true; }

    // A re-opened (or grown) source may reach further than the old one did.
    void resetTailEnd() noexcept { tailEnd = libav::noPts; }

    // One re-position per attempt after an end of stream that decoded nothing; a hardware fallback
    // retries the request and gets its own.
    void beginAttempt() noexcept { eofRetried = false; }

    // Arms the mode of the request about to be selected. Nearest-keyframe mode never skips frames
    // (the decoder must produce the keyframe itself); exact mode may skip non-reference frames that
    // provably end before the tolerance window, but only on a demuxer whose keyframe flags can be
    // trusted.
    void beginRequest (const RequestWindow& window, const SelectionContext& context) noexcept
    {
        keyframeOnly = window.isKeyframeMode();
        skipBeforeTs = libav::noPts;

        if (! keyframeOnly && context.packetReader.areKeyFlagsReliable())
        {
            const std::int64_t start = window.getStart();
            const std::int64_t margin =
                context.keyframes.getReorderTicks() + Positioner::getFrameTicks (context.source.getStreamInfo(), 2);
            skipBeforeTs = start > std::numeric_limits<std::int64_t>::min() + margin ? start - margin : start;
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
    [[nodiscard]] bool hasSkipWindow() const noexcept { return skipBeforeTs != libav::noPts; }

    [[nodiscard]] std::int64_t getFrameTs (const AVFrame& frame, const StreamInfo& stream) const noexcept
    {
        if (frame.best_effort_timestamp != libav::noPts) return frame.best_effort_timestamp;
        if (frame.pts != libav::noPts) return frame.pts;
        if (frame.pkt_dts != libav::noPts) return frame.pkt_dts;
        return frontier.synthTs != libav::noPts ? frontier.synthTs : stream.startPts;
    }

    [[nodiscard]] static std::int64_t getFrameDuration (const AVFrame& frame, const StreamInfo& stream) noexcept
    {
        return frame.duration > 0 ? frame.duration : stream.frameDurationHint;
    }

    // ---- answering without repositioning -----------------------------------------------------

    // Fast path: the frame covering the target is already held and nothing later covers it instead.
    // Empty when the request has to position. In nearest-keyframe mode the held frame must be the
    // keyframe the index says covers the target; in exact mode the look-ahead frame or the end of
    // stream must rule out a later frame.
    //
    // Runs before beginRequest(): the end-of-stream answer is formed under the previous request's
    // mode.
    [[nodiscard]] std::optional<std::expected<Selected, Error>>
    answerFromHeld (const RequestWindow& window, const SelectionContext& context, const CancelToken& token)
    {
        const StreamInfo& stream = context.source.getStreamInfo();
        const std::int64_t target = window.getTarget();

        if (! isHeldFrameCovering (target, stream)) return std::nullopt;
        if (window.isKeyframeMode())
        {
            const std::int64_t heldTs = getFrameTs (*held.getFrame(), stream);

            if ((held.getFrame()->flags & AV_FRAME_FLAG_KEY) != 0
                && context.keyframes.doesKeyCover (heldTs, target, containerIndexOf (context.source)))
            {
                return verifyKeyframeTail (Selected{ held.getFrame(), window.getAdjustment(), held.isConcealed() },
                                           window, context, token);
            }

            return std::nullopt;
        }

        if (pending.isValid() && target < getFrameTs (*pending.getFrame(), stream))
        {
            return Selected{ held.getFrame(), window.getAdjustment(), held.isConcealed() };
        }

        if (frontier.eof && ! pending.isValid() && isHeldFrameAtTail (stream))
        {
            auto answer = finishAtEof (target, context);

            if (! answer) return std::unexpected (std::move (answer.error()));
            if (answer->adjustment == Adjustment::none) answer->adjustment = window.getAdjustment();
            return *answer;
        }

        return std::nullopt;
    }

    // Whether the look-ahead frame can become the held frame and the decode continue from there.
    //
    // The invariant: the pending frame begins at or before `target`, so the frame on screen at
    // `target` is it or one the decoder has yet to produce; every frame between it and `target` will
    // be produced; and reaching `target` that way costs less than seeking to it.
    [[nodiscard]] bool canPromotePendingFrame (std::int64_t target, const SelectionContext& context) const noexcept
    {
        if (! pending.isValid() || frontier.drained) return false;
        const std::int64_t pendingTs = getFrameTs (*pending.getFrame(), context.source.getStreamInfo());
        return target >= pendingTs && ! frontier.skipped.containsIn (pendingTs, target)
               && context.positioner.isForwardCheaperThanSeek (target, makePositioningView (context));
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
    [[nodiscard]] bool canContinueFromHeldFrame (std::int64_t target, const SelectionContext& context) const noexcept
    {
        if (! held.isValid() || pending.isValid() || frontier.eof || frontier.drained) return false;
        const std::int64_t heldTs = getFrameTs (*held.getFrame(), context.source.getStreamInfo());
        return heldTs <= target && ! frontier.skipped.containsIn (heldTs, target)
               && context.positioner.isForwardCheaperThanSeek (target, makePositioningView (context));
    }

    // ---- nearest-keyframe mode ---------------------------------------------------------------

    // Whether the keyframe packet positioning left pending precedes the first presented frame: an
    // edit list trimmed its GOP, so it cannot be the answer.
    [[nodiscard]] bool isPreEditKeyframe (const SelectionContext& context) const noexcept
    {
        if (! context.packetReader.isPacketPending()) return false;
        const AVPacket& packet = context.packetReader.getPacket();
        return (packet.flags & AV_PKT_FLAG_DISCARD) != 0
               || (context.firstFrameTs != libav::noPts && packet.pts != libav::noPts
                   && packet.pts < context.firstFrameTs);
    }

    // Feeds the pending keyframe packet and drains, so the decoder emits that one frame at once
    // instead of after a pipeline's worth of packets (frame threads hold ~thread_count packets).
    // The decoder must be flushed before it is fed again (`frontier.drained`). A packet that cannot
    // be singled out as a keyframe is left to the normal decode.
    [[nodiscard]] std::expected<void, Error> feedKeyframeAndDrain (const SelectionContext& context) noexcept
    {
        if (! context.packetReader.areKeyFlagsReliable() || context.packetReader.getPacket().pts == libav::noPts)
            return {};
        context.position.noteKeyframeFed();
        const int sendResult = sendHeldPacket (context);

        if (sendResult < 0 && sendResult != libav::eagain)
        {
            return fail (ErrorCode::decodeFailed, sendResult, "avcodec_send_packet (keyframe)");
        }

        context.decoder.startDrain();
        frontier.draining = true;
        frontier.drained = true;
        frontier.landingKnown = true;
        return {};
    }

    // Nearest-keyframe mode answers with the keyframe at or before the target without decoding past
    // it, so it cannot tell a long GOP from the tail of a truncated file: a request past the end
    // would come back as the last keyframe, unflagged, where exact mode reports timeOutOfRange. When
    // the chosen keyframe is more than a GOP behind the request, read ahead (packets only) until one
    // proves the stream reaches the target or the source ends. Remembered in tailEnd: once per source.
    [[nodiscard]] std::expected<Selected, Error> verifyKeyframeTail (Selected selected, const RequestWindow& window,
                                                                     const SelectionContext& context,
                                                                     const CancelToken& token)
    {
        const StreamInfo& stream = context.source.getStreamInfo();
        const std::int64_t target = window.getTarget();

        if (selected.frame == nullptr) return selected;
        const std::int64_t frameTs = getFrameTs (*selected.frame, stream);
        const std::int64_t gopTicks =
            std::max ({ context.keyframes.getGopHint(), Positioner::getFrameTicks (stream, 2), std::int64_t{ 1 } });

        if (target <= frameTs + gopTicks || target <= context.packetReader.getVerifiedTo()) return selected;
        if (tailEnd == libav::noPts)
        {
            // The demuxer sits just after the chosen keyframe: read on until the question is answered.
            for (;;)
            {
                const int result =
                    context.packetReader.readVideoPacket (context.source, context.keyframes, frontier.tainted, token);

                if (result == libav::exitRequested)
                {
                    if (token.isRequested()) return fail (ErrorCode::cancelled, "cancelled");
                    return selected; // an I/O hiccup is not proof of anything: keep the keyframe
                }

                if (result == libav::eof)
                {
                    const std::int64_t verifiedTo = context.packetReader.getVerifiedTo();
                    const std::int64_t oneFrame = std::max<std::int64_t> (stream.frameDurationHint, 1);
                    tailEnd = (verifiedTo != libav::noPts ? verifiedTo : frameTs) + oneFrame;
                    break;
                }

                context.packetReader.unrefPacket();
                const std::int64_t verifiedTo = context.packetReader.getVerifiedTo();

                if (verifiedTo != libav::noPts && verifiedTo > target) break;
            }

            context.position.markInvalid(); // the demuxer has moved; the next request repositions
        }

        if (tailEnd == libav::noPts || target < tailEnd) return selected;
        if (context.outOfRange == OutOfRangePolicy::clampToLastFrame)
        {
            selected.adjustment = Adjustment::clampedToLast;
            return selected;
        }

        return fail (ErrorCode::timeOutOfRange,
                     "requested time is past the last frame ("
                         + toString (Time::fromTimestamp (std::max<std::int64_t> (tailEnd - stream.startPts, 0),
                                                          fromAv (stream.timeBase)))
                         + ")");
    }

    // ---- the selection loop ------------------------------------------------------------------

    // Core selection loop. Precondition: positioned (or continuing forward). Runs until a frame
    // answers the window, the request fails, or the demuxer has to be moved, in which case the step
    // says how (Step::Action) and the caller comes back here once it has.
    [[nodiscard]] Step select (const RequestWindow& window, const SelectionContext& context, const CancelToken& token)
    {
        for (;;)
        {
            if (token.isRequested()) return finished (fail (ErrorCode::cancelled, "cancelled"));
            const int result = receiveOne (context.decoder);

            if (result == 0)
            {
                if (auto step = onFrameReceived (window, context)) return *step;
                continue;
            }

            if (result == libav::eof) return onEndOfStream (window, context);
            if (result == libav::eagain)
            {
                if (auto step = feedNextPacket (context, token)) return *step;
                continue;
            }

            if (auto step = onDecodeError (result, context)) return *step;
        }
    }

private:
    // What the loop knows about the frame the decoder just produced.
    struct ReceivedFrame
    {
        std::int64_t ts{ libav::noPts };
        bool isKey{ false };
        bool concealed{ false }; // the decoder concealed errors in it, or its reference chain is suspect
    };

    // What placing a received frame against the request concluded.
    enum class Placement
    {
        answered,     // the held frame is the answer
        keepDecoding, // the answer lies further on
        overshot,     // the first frame out is already past the target
    };

    static constexpr int maxConsecutiveErrors = 32;

    [[nodiscard]] static Step finished (std::expected<Selected, Error> result) noexcept
    {
        return Step{ Step::Action::done, std::move (result), libav::noPts };
    }

    [[nodiscard]] static Step backOff (std::int64_t observedKey) noexcept
    {
        return Step{ Step::Action::backOff, Selected{}, observedKey };
    }

    [[nodiscard]] Selected heldAsAnswer() const noexcept
    {
        return Selected{ held.getFrame(), Adjustment::none, held.isConcealed() };
    }

    [[nodiscard]] PositioningView makePositioningView (const SelectionContext& context) const noexcept
    {
        return PositioningView{
            context.source.getStreamInfo(),   context.keyframes, frontier, context.costs, context.position,
            containerIndexOf (context.source)
        };
    }

    // The received frame becomes the answer. The look-ahead slot is emptied with it: no frame past
    // the new held one has been seen yet, and a stale one there would claim it covers the request.
    void holdReceived (VideoDecoder& decoder, bool concealed) noexcept
    {
        held.adopt (decoder.getFrame(), concealed);
        pending.clear();
    }

    // One frame of look-ahead past the held frame: what proves the held frame covers the request.
    void pendReceived (VideoDecoder& decoder, bool concealed) noexcept
    {
        pending.adopt (decoder.getFrame(), concealed);
    }

    // ---- one pass of the loop, by what the decoder returned ----------------------------------

    // Accounts for the frame the decoder just produced and decides whether it, or the frame held
    // before it, answers the request. Empty when the loop has to keep decoding.
    [[nodiscard]] std::optional<Step> onFrameReceived (const RequestWindow& window, const SelectionContext& context)
    {
        const StreamInfo& stream = context.source.getStreamInfo();
        decodeErrors = 0;
        context.costs.noteFirstFrame();

        if (stream.synthesizeTimestamps) stampSynthesisedTimestamps (context.decoder.getFrame(), stream);
        const AVFrame& received = context.decoder.getFrame();
        ReceivedFrame frame;
        frame.ts = getFrameTs (received, stream);
        frame.isKey = (received.flags & AV_FRAME_FLAG_KEY) != 0;
        frontier.synthTs = frame.ts + getFrameDuration (received, stream);
        frontier.lastReceivedTs = frame.ts;
        ++frontier.receivedSinceSeek;
        // GOP length estimate (a lower bound, exact at the next keyframe): feeds the back-off step
        // and the forward-scan limit.
        context.keyframes.noteKeyframeSpan (frontier.lastKeyTs, frame.ts);

        if (frame.isKey) frontier.lastKeyTs = frame.ts;
        if ((received.flags & AV_FRAME_FLAG_CORRUPT) != 0)
        {
            frontier.skipped.record (frame.ts); // not produced: nothing may decode across it
            // Stashed only as a fallback for a stream that never produces a better frame (finishAtEof).
            corruptLast.adopt (context.decoder.getFrame(), /*wasConcealed=*/true);
            return std::nullopt;
        }

        if (frame.isKey) frontier.tainted = false;
        frame.concealed = received.decode_error_flags != 0 || frontier.tainted;
        frontier.lastEnd = std::max (frontier.lastEnd, frame.ts + getFrameDuration (received, stream));
        frontier.skipped.clearAt (frame.ts);
        const Placement placement =
            keyframeOnly ? placeInKeyframeMode (frame, window, context) : placeInExactMode (frame, window, context);
        switch (placement)
        {
        case Placement::answered:
            return finished (heldAsAnswer());
        case Placement::keepDecoding:
            return std::nullopt;
        case Placement::overshot:
            break;
        }

        return handleOvershoot (frame, window, context);
    }

    // Containers that carry no timestamps (raw elementary streams): libavformat's own counters run in
    // decode order, which disagrees with display order under B-frame reordering, so the loop stamps
    // frames itself, in output order, a frame duration apart.
    void stampSynthesisedTimestamps (AVFrame& received, const StreamInfo& stream) const noexcept
    {
        const std::int64_t synthesised = frontier.synthTs != libav::noPts ? frontier.synthTs : stream.startPts;
        received.pts = synthesised;
        received.best_effort_timestamp = synthesised;
        received.duration = stream.frameDurationHint;
    }

    // "The keyframe at or before the target". A keyframe at or before it is held (a non-keyframe only
    // as a fallback) and is the answer when it is provably the covering one; otherwise decoding goes
    // on until a frame past the target shows up, and the frame held then is the answer.
    [[nodiscard]] Placement placeInKeyframeMode (const ReceivedFrame& frame, const RequestWindow& window,
                                                 const SelectionContext& context) noexcept
    {
        if (frame.ts <= window.getTarget())
        {
            if (frame.isKey || ! held.isValid()) holdReceived (context.decoder, frame.concealed);
            if (frame.isKey && isKeyframeTheAnswer (frame.ts, window.getTarget(), context)) return Placement::answered;
            return Placement::keepDecoding;
        }

        if (held.isValid())
        {
            pendReceived (context.decoder, frame.concealed);
            return Placement::answered;
        }

        return Placement::overshot;
    }

    // Whether a keyframe at `frameTs` is known to be the one covering `target`: it was fed after a
    // packet-level landing that established it, or it is the first frame out of a seek aimed at the
    // target itself. After a back-off (the seek aimed below the target) neither holds, and the loop
    // has to see a frame past the target before it can answer.
    [[nodiscard]] bool isKeyframeTheAnswer (std::int64_t frameTs, std::int64_t target,
                                            const SelectionContext& context) const noexcept
    {
        if (frontier.landingKnown || frameTs == target) return true;
        return frontier.receivedSinceSeek == 1 && context.position.getSeekTarget() == target;
    }

    // Exact mode: a frame before the target is held and answers at once when it is already inside
    // the tolerance window; the frame at the target answers; the first frame past the target proves
    // the held one covers it.
    [[nodiscard]] Placement placeInExactMode (const ReceivedFrame& frame, const RequestWindow& window,
                                              const SelectionContext& context) noexcept
    {
        if (frame.ts < window.getTarget())
        {
            holdReceived (context.decoder, frame.concealed);
            return frame.ts >= window.getStart() ? Placement::answered : Placement::keepDecoding;
        }

        if (frame.ts == window.getTarget())
        {
            holdReceived (context.decoder, frame.concealed);
            return Placement::answered;
        }

        if (held.isValid())
        {
            pendReceived (context.decoder, frame.concealed);
            return Placement::answered;
        }

        return Placement::overshot;
    }

    // The first frame out is already past the target. Either the target precedes the first frame of
    // the stream (provable only after the explicit start seek) or the seek landed late. A late
    // landing is backed off even when a later frame would satisfy the `after` tolerance: the frame
    // actually on screen at the target exists until proven otherwise.
    [[nodiscard]] Step handleOvershoot (const ReceivedFrame& frame, const RequestWindow& window,
                                        const SelectionContext& context) noexcept
    {
        const StreamInfo& stream = context.source.getStreamInfo();
        const bool isAtStart = context.position.isLandedAtStart() || ! stream.seekable
                               || (context.firstFrameTs != libav::noPts && frame.ts <= context.firstFrameTs);
        const bool isLanding = frontier.receivedSinceSeek == 1;
        const std::int64_t observedKey = frame.isKey ? frame.ts : libav::noPts;

        if (isLanding && ! isAtStart) return backOff (observedKey);
        if (frame.ts <= window.getEnd())
        {
            holdReceived (context.decoder, frame.concealed);
            return finished (heldAsAnswer());
        }

        if (isAtStart)
        {
            holdReceived (context.decoder, frame.concealed);
            return finished (Selected{ held.getFrame(), Adjustment::clampedToFirst, held.isConcealed() });
        }

        return backOff (observedKey);
    }

    // The decoder has nothing more to give. Decides whether that is the answer, a landing in the
    // tail that needs a back-off, or a spurious end of stream worth one retry.
    [[nodiscard]] Step onEndOfStream (const RequestWindow& window, const SelectionContext& context)
    {
        if (shouldBackOffAtEndOfStream (context)) return backOff (libav::noPts);
        if (shouldRetryEmptyEndOfStream (context))
        {
            eofRetried = true;
            return Step{ Step::Action::retryAfterEmptyEof, Selected{}, libav::noPts };
        }

        frontier.eof = true;

        if (frontier.lastEnd != std::numeric_limits<std::int64_t>::min())
        {
            tailEnd = tailEnd == libav::noPts ? frontier.lastEnd : std::max (tailEnd, frontier.lastEnd);
        }

        return finished (finishAtEof (window.getTarget(), context));
    }

    // Nothing decodable came out between the landing and the end of the stream: the seek landed in
    // the tail. Backing off is pointless once the start seek has been done, because then the stream
    // really has no frame for the target, and impossible on a drained or unpositioned decoder.
    [[nodiscard]] bool shouldBackOffAtEndOfStream (const SelectionContext& context) const noexcept
    {
        if (held.isValid() || corruptLast.isValid() || frontier.drained) return false;
        const StreamInfo& stream = context.source.getStreamInfo();
        return stream.seekable && context.position.isPositioned() && ! context.position.isLandedAtStart();
    }

    // Not one frame came out of the decoder since this request positioned. On a seekable source that
    // says nothing about the stream: the start seek landed and the very first read reported the end,
    // which is what libavio's sticky eof_reached does after a cancellation (a seek does not clear
    // it). Worth clearing and re-positioning once before concluding the stream ended; the
    // landed-at-start test above would otherwise accept that first read as proof.
    [[nodiscard]] bool shouldRetryEmptyEndOfStream (const SelectionContext& context) const noexcept
    {
        if (held.isValid() || corruptLast.isValid() || frontier.drained || eofRetried) return false;
        return context.source.getStreamInfo().seekable && frontier.receivedSinceSeek == 0;
    }

    // The decoder wants input. Feeds the next packet; a failure to do so ends the request.
    [[nodiscard]] std::optional<Step> feedNextPacket (const SelectionContext& context, const CancelToken& token)
    {
        const int result = feedOne (context, token);

        if (result == 0 || result == libav::eof) return std::nullopt;
        if (result == libav::exitRequested)
        {
            if (token.isRequested()) return finished (fail (ErrorCode::cancelled, "cancelled"));
            return finished (
                fail (ErrorCode::decodeFailed, result, "av_read_frame: interrupted without a cancellation"));
        }

        if (context.decoder.isHardwareFault (result))
        {
            context.decoder.noteHardwareFault();
            return finished (fail (ErrorCode::decodeFailed, result, "avcodec_send_packet (hardware)"));
        }

        return finished (fail (ErrorCode::decodeFailed, result, "avcodec_send_packet"));
    }

    // avcodec_receive_frame failed. Corrupt input is absorbed up to a limit; a hardware fault ends
    // the request so the owner's fallback ladder can act on it.
    [[nodiscard]] std::optional<Step> onDecodeError (int result, const SelectionContext& context) noexcept
    {
        ++totalDecodeErrors;
        frontier.tainted = true;

        if (context.decoder.isHardwareFault (result))
        {
            context.decoder.noteHardwareFault();
            return finished (fail (ErrorCode::decodeFailed, result, "avcodec_receive_frame (hardware)"));
        }

        if (++decodeErrors > maxConsecutiveErrors)
        {
            return finished (
                fail (ErrorCode::decodeFailed, result, "avcodec_receive_frame: too many consecutive errors"));
        }

        return std::nullopt;
    }

    // ---- feeding the decoder -----------------------------------------------------------------

    // Pulls one frame into the decoder's frame. Returns 0, libav::eof, libav::eagain (needs a packet)
    // or an error.
    //
    // The one thing added to VideoDecoder::receive(): once the drain has started, "nothing yet" is
    // the end of the stream rather than a request for another packet. Whether a drain is in
    // progress is the decode loop's own state, so the translation is made here and not inside the
    // decoder.
    [[nodiscard]] int receiveOne (VideoDecoder& decoder) noexcept
    {
        const int result = decoder.receive();

        if (result == libav::eagain && frontier.draining) return libav::eof;
        return result;
    }

    // Sends the reader's live packet. Returns 0 (consumed), libav::eagain (kept for a retry after the
    // decoder has been drained) or an error (packet dropped).
    [[nodiscard]] int sendHeldPacket (const SelectionContext& context) noexcept
    {
        AVPacket& packet = context.packetReader.getPacket();
        const std::int64_t fedDts = packet.dts;
        const std::int64_t fedPts = packet.pts;
        const int sendResult = context.decoder.send (packet);

        if (sendResult == libav::eagain)
        {
            context.packetReader.holdPacketForRetry();
            return sendResult;
        }

        if (sendResult == 0)
        {
            if (fedDts != libav::noPts) frontier.lastFedDts = fedDts;
            if (fedPts != libav::noPts)
            {
                frontier.lastFedPts =
                    frontier.lastFedPts == libav::noPts ? fedPts : std::max (frontier.lastFedPts, fedPts);
            }
        }

        context.packetReader.releasePacket();
        return sendResult;
    }

    // Reads the next packet of our stream and feeds it. Returns 0, libav::eof (drain started),
    // libav::exitRequested (interrupted) or a decoder error.
    // Not noexcept: the skipped-frame record below it allocates.
    [[nodiscard]] int feedOne (const SelectionContext& context, const CancelToken& token)
    {
        if (context.packetReader.isPacketPending())
        {
            if ((context.packetReader.getPacket().flags & AV_PKT_FLAG_KEY) != 0) context.position.noteKeyframeFed();
            prepareSend (context);
            const int sendResult = sendHeldPacket (context);

            if (sendResult == libav::eagain)
            {
                // Both receive and send report EAGAIN: the decoder violates its contract.
                context.packetReader.releasePacket();
                return libav::einval;
            }

            if (sendResult == 0) return 0;
            if (sendResult != libav::invalidData) return sendResult;
            ++totalDecodeErrors;
            frontier.tainted = true;
        }

        // Drained for a keyframe-only decode: only a seek (which flushes) may feed it again.
        if (frontier.drained) return libav::eof;
        for (;;)
        {
            const int result =
                context.packetReader.readVideoPacket (context.source, context.keyframes, frontier.tainted, token);

            if (result == libav::exitRequested) return result;
            if (result == libav::eof)
            {
                frontier.draining = true;
                context.decoder.startDrain();
                return libav::eof;
            }

            const bool keyPacket = (context.packetReader.getPacket().flags & AV_PKT_FLAG_KEY) != 0;

            if (context.position.isAwaitingKey() && context.packetReader.areKeyFlagsReliable() && ! keyPacket)
            {
                // Mid-GOP after a seek: the decoder would decode these in full and drop them anyway.
                context.packetReader.unrefPacket();
                continue;
            }

            if (keyPacket) context.position.noteKeyframeFed();
            prepareSend (context);
            const int sendResult = sendHeldPacket (context);

            if (sendResult == 0 || sendResult == libav::eagain) return 0;
            if (sendResult == libav::invalidData)
            {
                ++totalDecodeErrors;
                frontier.tainted = true;

                if (++decodeErrors > maxConsecutiveErrors) return sendResult;
                continue;
            }

            return sendResult;
        }
    }

    // Before a packet goes to the decoder: non-reference frames far before the target are skipped
    // at the decoder level (AVDISCARD_NONREF), and recorded as holes in case the decoder drops them.
    void prepareSend (const SelectionContext& context)
    {
        const AVPacket& packet = context.packetReader.getPacket();
        AVDiscard skip = AVDISCARD_DEFAULT;

        if (canSkipNonReferenceFrame (packet, context))
        {
            skip = AVDISCARD_NONREF;
            frontier.skipped.record (packet.pts);
        }

        context.decoder.setSkipPolicy (skip);
    }

    // A frame may be skipped only when its display interval provably ends before the window.
    // Durations cannot prove that (mov stores decode-order deltas, so a VFR stream's frame may say
    // "1/30 s"), nor can the average frame rate. What does: a frame with a *later* presentation
    // time has already been fed (B-frames follow their forward reference in decode order) and that
    // later time is itself before the window. Only on a demuxer whose keyframe flags can be trusted,
    // and never for a keyframe.
    [[nodiscard]] bool canSkipNonReferenceFrame (const AVPacket& packet, const SelectionContext& context) const noexcept
    {
        if (skipBeforeTs == libav::noPts || ! context.packetReader.areKeyFlagsReliable()) return false;
        if ((packet.flags & AV_PKT_FLAG_KEY) != 0 || packet.pts == libav::noPts) return false;
        if (frontier.lastFedPts == libav::noPts) return false;
        return packet.pts < frontier.lastFedPts && frontier.lastFedPts <= skipBeforeTs;
    }

    // ---- end of stream -----------------------------------------------------------------------

    // The answer once the stream has ended: the held frame if `target` falls inside its display
    // interval (recomputed from the frame, never remembered as a flag), the stashed corrupt frame as
    // a last resort, otherwise the failure that says why nothing could be produced.
    [[nodiscard]] std::expected<Selected, Error> finishAtEof (std::int64_t target, const SelectionContext& context)
    {
        const StreamInfo& stream = context.source.getStreamInfo();
        const auto finish = [&] (AVFrame* frame, bool corrupt) -> std::expected<Selected, Error>
        {
            const std::int64_t frameTs = getFrameTs (*frame, stream);
            // In keyframe mode the stream extends past the held keyframe to the last frame decoded
            // after it.
            const std::int64_t displayEnd =
                std::max (frameTs + getFrameDuration (*frame, stream), keyframeOnly ? frontier.lastEnd : frameTs);

            if (target < displayEnd) return Selected{ frame, Adjustment::none, corrupt };
            if (context.outOfRange == OutOfRangePolicy::clampToLastFrame)
                return Selected{ frame, Adjustment::clampedToLast, corrupt };
            return fail (ErrorCode::timeOutOfRange,
                         "requested time is past the last frame ("
                             + toString (Time::fromTimestamp (std::max<std::int64_t> (frameTs - stream.startPts, 0),
                                                              fromAv (stream.timeBase)))
                             + ")");
        };

        if (held.isValid()) return finish (held.getFrame(), held.isConcealed());
        if (corruptLast.isValid())
        {
            held.adopt (corruptLast);

            // Nearest-keyframe mode asks for "the keyframe at or before the target", and a stashed frame
            // at or before it answers that: its display interval is not the question, as it is in exact
            // mode.
            if (keyframeOnly && target >= getFrameTs (*held.getFrame(), stream))
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
        if (! held.isValid()) return false;
        const std::int64_t heldTs = getFrameTs (*held.getFrame(), stream);
        return heldTs <= target && ! frontier.skipped.containsIn (heldTs, target);
    }

    FrameSlot held;
    FrameSlot pending;
    FrameSlot corruptLast;
    DecodeFrontier frontier;    // where the decoder is; every reposition resets it
    bool eofRetried{ false };   // one re-position per request after an end of stream that decoded nothing
    bool keyframeOnly{ false }; // the current request is in nearest-keyframe mode
    std::int64_t skipBeforeTs{ libav::noPts }; // packets whose frames end before this may skip non-reference frames
    std::int64_t tailEnd{ libav::noPts };      // end of the data once the end of stream has been observed
    int decodeErrors{ 0 };                     // consecutive, since the last frame that came out
    std::int64_t totalDecodeErrors{ 0 };
};

} // namespace stills::detail

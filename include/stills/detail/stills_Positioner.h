#pragma once
// stills/detail/stills_Positioner.h — where the decoder is, and every decision about where to send it.
//
// Two types. `Position` is where the demuxer was last put and what is known about that landing.
// `Positioner` decides whether a request can be answered by decoding on, and when it cannot, where
// to aim the seek and where to aim the next one after that landing proved wrong.
//
// Positioner does not seek, and holds no reference to anything. It is the same answer
// PacketReader::readLanding() reached, for the same reason: the scan chooses a keyframe and
// returns "go back to this one" as data, because going back is a seek and seeks are not the
// reader's. Here the reason is larger. A seek is not a call, it is the invalidation of every other
// type's state — FramePipeline::afterSeek() flushes the decoder, empties three frame slots,
// resets the frontier, resets the reader's position and the index's contiguity, and only then
// records where it aimed. A type that performed seeks would have to reach into five other objects,
// which is the orchestrator's job by definition. So the decisions live here and the acting lives
// with the owner, and the dependency runs one way: FramePipeline -> Positioner.
//
// What a decision needs to read comes in as a `PositioningView`, built at the call and never
// stored. Same reasoning as KeyframeIndex's ContainerIndex: a re-open replaces the AVStream and
// rewrites the stream facts, so nothing here may outlive the call it was handed. No decision in
// this file re-opens anything, so the references in a view cannot go stale inside one.
//
// Timestamps are in the stream's own time base throughout, as everywhere else in the pipeline.
//
// Not thread-safe: a FramePipeline is single-threaded by contract (stills_FramePipeline.h), and its
// Positioner is only ever touched by the thread running that pipeline's decode loop.

#include <algorithm>
#include <cstdint>
#include <limits>

#include "stills/detail/stills_DecodeFrontier.h"
#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_KeyframeIndex.h"
#include "stills/detail/stills_MediaSource.h"
#include "stills/detail/stills_SeekCostModel.h"

namespace stills::detail
{

// Where the demuxer was put, and what that landing is known to be.
//
// The invariant the type exists for: a landing is established by one transition, never by writing
// the fields one at a time. Every transition is a verb with a contract, so "positioned but still
// awaiting a keyframe from a seek that never happened" is not a state a caller can reach by
// forgetting a line.
//
// `seekTarget` is meaningful only while `positioned`; the accessors do not enforce that because
// every reader already tests `isPositioned()` (or is inside a landing that has just set it).
class Position
{
public:
    // Whether the demuxer is somewhere known. False means the next request must seek before it can
    // conclude anything: an interrupted read, a failed seek, or a loop that moved the demuxer
    // behind the decoder's back.
    [[nodiscard]] bool isPositioned() const noexcept { return positioned; }
    // The landing was the explicit start seek or a re-open, so nothing lies before it. What lets a
    // first frame past the request be accepted as "the stream starts here" rather than backed off.
    [[nodiscard]] bool isLandedAtStart() const noexcept { return landedAtStart; }
    // No keyframe packet has been fed since the landing, so mid-GOP packets are worth skipping:
    // the decoder would decode them in full and drop them anyway.
    [[nodiscard]] bool isAwaitingKey() const noexcept { return awaitingKey; }
    // What the last seek aimed at — not where it landed, which only the first packet after it can
    // say.
    [[nodiscard]] std::int64_t getSeekTarget() const noexcept { return seekTarget; }

    // A seek has been issued at `target`. A seek always leaves a keyframe to be awaited: what it
    // landed on is unknown until a packet says, and mid-GOP packets before that one are waste.
    void markSeeked (std::int64_t target, bool atStart) noexcept
    {
        positioned = true;
        awaitingKey = true;
        landedAtStart = atStart;
        seekTarget = target;
    }

    // The demuxer is at the first packet of a freshly opened source and was never seeked there.
    // Deliberately not markSeeked(): nothing has been fed, so there is no keyframe to await, and
    // setting that flag here would make the pipeline skip packets ahead of the stream's own first
    // one on a demuxer whose first packet is not flagged. Only the open-time probe reaches this.
    void markAtStart (std::int64_t startPts) noexcept
    {
        positioned = true;
        landedAtStart = true;
        seekTarget = startPts;
    }

    // A non-rewindable source after an interrupt: the demuxer is at or after the last frame that
    // came out, which is the best that can be said and is still true. Everything else about the
    // landing was destroyed with the decoder's buffers.
    void markCarriedForward() noexcept
    {
        positioned = true;
        landedAtStart = false;
    }

    // Where the demuxer is is no longer known. The next request seeks.
    void markInvalid() noexcept { positioned = false; }

    // The landing scan concluded that the decoder must not be fed before a keyframe arrives.
    void markAwaitingKey() noexcept { awaitingKey = true; }
    // A keyframe packet has been fed, so packets after it are the decoder's to use.
    void noteKeyframeFed() noexcept { awaitingKey = false; }

private:
    bool positioned{ false };
    bool landedAtStart{ false };
    bool awaitingKey{ false };
    std::int64_t seekTarget{ 0 };
};

// What a positioning decision reads, gathered at the call site. Built per call and never stored.
struct PositioningView
{
    const StreamInfo& stream;
    const KeyframeIndex& keys;
    const DecodeFrontier& frontier;
    const SeekCostModel& costs;
    const Position& position;
    ContainerIndex container; // only the trusted-index queries look at it
};

class Positioner
{
public:
    // One second, and n frames, in stream ticks. n frames is zero on a stream with no nominal frame
    // duration, which every caller reads as "no margin can be computed".
    [[nodiscard]] static std::int64_t oneSecond (const StreamInfo& s) noexcept
    {
        return av_rescale_q (1, AVRational{ 1, 1 }, s.timeBase);
    }

    [[nodiscard]] static std::int64_t framesTicks (const StreamInfo& s, int n) noexcept
    {
        return s.frameDurationHint > 0 ? n * s.frameDurationHint : 0;
    }

    // Starts a fresh back-off ladder: a new request, or a promotion that proved the pipeline is
    // already where it needs to be.
    void resetBackoff() noexcept
    {
        backoffs = 0;
        backoffStep = 0;
    }

    // Whether the decoder can still reach `target` by decoding on from where it is.
    //
    // The invariant: continuing forward answers with frames the decoder has yet to produce, so it
    // is sound only while every frame between the decoder's frontier and `target` will actually
    // come out. A gap there — a non-reference frame skipped for a request that was abandoned before
    // it got there — would be the frame on screen, and nothing may be concluded across one.
    [[nodiscard]] static bool canDecodeForwardTo (std::int64_t target, const PositioningView& v) noexcept
    {
        return v.position.isPositioned() && ! v.frontier.drained && ! v.frontier.eof
               && v.frontier.lastReceivedTs != k::noPts && target >= v.frontier.lastReceivedTs
               && ! v.frontier.skipped.containsIn (v.frontier.lastReceivedTs, target);
    }

    // A landing nothing has been decoded from yet, on a source that cannot seek: it is at the start
    // of the stream, which is the only place a request that cannot seek may begin.
    [[nodiscard]] static bool isUndecodedStartLanding (const PositioningView& v) noexcept
    {
        return hasDecodedNothingSinceLanding (v) && v.position.isLandedAtStart();
    }

    // A landing nothing has been decoded from yet, aimed at or just before `target`: close enough
    // that seeking again would land in the same place. select() verifies it against the first frame.
    [[nodiscard]] bool isUndecodedLandingBefore (std::int64_t target, const PositioningView& v) const noexcept
    {
        return hasDecodedNothingSinceLanding (v) && v.position.getSeekTarget() <= target
               && target - v.position.getSeekTarget() <= getForwardScanLimit (v);
    }

    // Without a trusted index, decode forward when the target is within one GOP (at least 3 s):
    // a seek would land in the same or the next GOP and decode about as many frames anyway.
    [[nodiscard]] std::int64_t getForwardScanLimit (const PositioningView& v) const noexcept
    {
        const std::int64_t threeSeconds = av_rescale_q (3, AVRational{ 1, 1 }, v.stream.timeBase);
        return std::max (threeSeconds, v.keys.getGopHint());
    }

    // True when decoding forward to `target` is cheaper than seeking: the keyframe at or before the
    // target has already been fed to the decoder (both sides in the container's own timestamp
    // domain), or the distance is short. Cost-aware for slow seeks (hardware): frames between the
    // current position and that keyframe are decoded forward while they cost less than a seek.
    [[nodiscard]] bool isForwardCheaperThanSeek (std::int64_t target, const PositioningView& v) const noexcept
    {
        std::int64_t keyTs = k::noPts; // the covering keyframe, in the same domain as fedTs
        std::int64_t fedTs = k::noPts;
        bool covered = false;

        if (v.stream.indexTrusted)
        {
            if (const AVIndexEntry* kf = v.keys.getIndexKeyBefore (target, v.container); kf != nullptr)
            {
                keyTs = kf->timestamp;
                fedTs = v.keys.pickIndexTs (v.frontier.lastFedDts, v.frontier.lastFedPts);
                covered = true;
            }
        }
        else if (const KeyEntry* e = v.keys.findCoveringKey (target); e != nullptr)
        {
            keyTs = e->dts != k::noPts ? e->dts : e->pts;
            fedTs = e->dts != k::noPts ? v.frontier.lastFedDts : v.frontier.lastFedPts;
            covered = true;
        }

        if (covered && keyTs != k::noPts && fedTs != k::noPts)
        {
            // The covering keyframe has been fed: the target is downstream of the decoder's state.
            if (keyTs <= fedTs + std::max<std::int64_t> (v.stream.frameDurationHint, 0)) return true;
            // A keyframe lies ahead: forward wins while the frames up to it cost less than a seek (a
            // real trade on hardware decoders and tiny frames).
            if (v.costs.getFrameCostMs() > 0 && v.stream.frameDurationHint > 0)
            {
                const double framesToKey =
                    static_cast<double> (keyTs - fedTs) / static_cast<double> (v.stream.frameDurationHint);
                return framesToKey * v.costs.getFrameCostMs() < v.costs.getSeekCostMs();
            }

            return false;
        }

        if (v.stream.indexTrusted)
            return target - v.frontier.lastReceivedTs
                   <= framesTicks (v.stream,
                                   4); // the index does not cover P (unread fragment): seeks are exact there
        return target - v.frontier.lastReceivedTs <= getForwardScanLimit (v);
    }

    // Where to aim a seek on a container with a trusted index. Fragmented MP4 is learned: its
    // demuxer applies no reorder shift, so later seek targets carry it and land right the first
    // time (`seekBias`, learned in retryAfterOvershoot()).
    [[nodiscard]] std::int64_t getIndexedAim (std::int64_t P) const noexcept { return P - seekBias; }

    // Where to aim on a container without a trusted index: one GOP early, plus room for the
    // reorder delay, so the scan starts before the keyframe covering P rather than after it.
    [[nodiscard]] static std::int64_t getScanAim (std::int64_t P, const PositioningView& v) noexcept
    {
        const std::int64_t margin = framesTicks (v.stream, 2) + v.keys.getReorderTicks();
        const std::int64_t gopHint = v.keys.getGopHint();
        return P - margin - (gopHint > 0 ? gopHint : 0);
    }

    // Where to aim next, and whether to give up and go to the start instead. Advances the ladder,
    // so it is called once per retry and its answer is acted on.
    struct Aim
    {
        std::int64_t target{ 0 };
        bool toStart{ false }; // the ladder is spent, or the target is at or before the stream's start
    };

    // The landing on a trusted index overshot: the first keyframe packet after it is past `P`.
    // Aims at the keyframe strictly before it, retreating by a whole GOP more on each further round.
    //
    // `keyPts`/`keyDts` are that keyframe packet's. The reorder delay is read from the packet when
    // it carries both (mov), and from the index otherwise (Matroska keyframes carry no dts).
    [[nodiscard]] Aim retryAfterOvershoot (std::int64_t P, std::int64_t keyPts, std::int64_t keyDts, int round,
                                           const PositioningView& v) noexcept
    {
        // Read after the landing, not before it: the packets it just read may have lowered the delay.
        const std::int64_t reorderTicks = v.keys.getReorderTicks();
        const std::int64_t delay = (keyDts != k::noPts && keyPts > keyDts) ? keyPts - keyDts : reorderTicks;

        if (keyDts != k::noPts && reorderTicks > 0 && keyDts > P - reorderTicks && seekBias == 0)
        {
            // The demuxer searched its DTS index with our PTS unshifted (fragmented MP4): remember the
            // reorder delay so the next seek aims right. Open-GOP overshoots (the CRA's DTS is already
            // below P - delay) must not set it, or every seek would land a GOP early.
            seekBias = reorderTicks;
        }

        if (round >= 3 || backoffs >= maxBackoffs) return Aim{ 0, true };
        const std::int64_t target =
            keyPts - 1 - delay
            - (round > 0 ? std::max ({ oneSecond (v.stream), v.keys.getGopHint(), std::int64_t{ 1 } }) * round : 0);
        ++backoffs;
        return Aim{ target, false };
    }

    // Where to retreat to after a landing proved to be past `P`. The new target is the observed
    // keyframe (or the last target) minus a doubling step (starting at the GOP length, at least one
    // second) and a reorder margin: the generic seek lands on the last packet with DTS <= target,
    // and a keyframe's DTS precedes its PTS, so aiming exactly at a keyframe lands just after it.
    [[nodiscard]] Aim nextBackoff (std::int64_t P, std::int64_t observedKey /* or k::noPts */,
                                   const PositioningView& v) noexcept
    {
        if (backoffStep <= 0) backoffStep = std::max ({ oneSecond (v.stream), v.keys.getGopHint(), std::int64_t{ 1 } });
        const std::int64_t margin = framesTicks (v.stream, 2) + v.keys.getReorderTicks();
        const std::int64_t lastTarget = v.position.getSeekTarget();
        const std::int64_t anchor = std::min (P, observedKey != k::noPts ? observedKey : lastTarget);
        std::int64_t earlier = anchor - backoffStep - margin;

        if (earlier >= lastTarget) earlier = lastTarget - backoffStep; // always strictly earlier than last time
        if (backoffStep < std::numeric_limits<std::int64_t>::max() / 2) backoffStep *= 2;
        if (++backoffs >= maxBackoffs || earlier <= v.stream.startPts) return Aim{ 0, true };
        return Aim{ earlier, false };
    }

private:
    // Positioned, with no frame out of the decoder since: the landing itself is all there is to go
    // on. `drained`/`eof` exclude a decoder that has nothing left to give without a flush.
    [[nodiscard]] static bool hasDecodedNothingSinceLanding (const PositioningView& v) noexcept
    {
        return v.position.isPositioned() && ! v.frontier.drained && ! v.frontier.eof
               && v.frontier.lastReceivedTs == k::noPts;
    }

    // Hard cap on landing back-offs per request; the doubling step reaches the start long before.
    static constexpr int maxBackoffs = 64;

    int backoffs{ 0 };
    std::int64_t backoffStep{ 0 };
    // Subtracted from seek targets on demuxers that search a DTS index with an unshifted PTS
    // (fragmented MP4). Learned once, from the first overshoot that proves it.
    std::int64_t seekBias{ 0 };
};

} // namespace stills::detail

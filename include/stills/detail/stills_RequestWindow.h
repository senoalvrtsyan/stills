#pragma once
// stills/detail/stills_RequestWindow.h — a request mapped into the stream's timestamp domain: the
// frame wanted, the tolerance window around it, and what the mapping already had to adjust.
//
// Pure arithmetic over StreamInfo, so it is unit-tested without a container (tests/test_request_window.cpp).
// Timestamps are in the stream's own time base.

#include <cstdint>
#include <expected>
#include <limits>
#include <string>

#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_MediaSource.h"
#include "stills/stills_Error.h"
#include "stills/stills_Image.h"
#include "stills/stills_Options.h"
#include "stills/stills_Time.h"

namespace stills::detail
{

class RequestWindow
{
public:
    // Maps `requested` (asset-relative) into stream ticks, applies the out-of-range policy and the
    // tolerance. Rejects non-finite and negative times as invalidArgument, and times the stream's
    // timestamp domain cannot represent as timeOutOfRange.
    [[nodiscard]] static std::expected<RequestWindow, Error>
    resolve (Time requested, Tolerance tolerance, const StreamInfo& stream, OutOfRangePolicy outOfRange)
    {
        if (! requested.isFinite())
        {
            return fail (ErrorCode::invalidArgument, "requested time is not finite: " + toString (requested));
        }

        if (requested.isNegative())
        {
            return fail (ErrorCode::invalidArgument, "requested time is negative: " + toString (requested));
        }

        // Nearest tick, not floor: coarse time bases (Matroska/WebM: 1 ms) store k/fps rounded to the
        // nearest tick, so an exact k/fps lands a fraction of a tick below frame k and flooring would
        // return frame k-1.
        const std::int64_t relative = requested.toTimestamp (fromAv (stream.timeBase), TimeRounding::nearest);

        if (relative == std::numeric_limits<std::int64_t>::max())
        {
            return fail (ErrorCode::timeOutOfRange, "requested time does not fit the stream time base");
        }

        std::int64_t target = 0;

        // A representable Time can still fall outside the stream's timestamp domain once the origin
        // is added (an MPEG-TS starting at 10 s). Report it rather than wrapping.
        if (__builtin_add_overflow (stream.startPts, relative, &target))
        {
            return fail (ErrorCode::timeOutOfRange, "requested time does not fit the stream time base");
        }

        Adjustment adjustment = Adjustment::none;

        // Containers occasionally understate their duration by a frame; give one frame of slack before
        // rejecting up front, and let the decoder (end of stream) decide inside that margin.
        if (stream.durationPts && relative > *stream.durationPts + std::max<std::int64_t> (stream.frameDurationHint, 0))
        {
            if (outOfRange == OutOfRangePolicy::error)
            {
                return fail (ErrorCode::timeOutOfRange,
                             toString (requested) + " is beyond the asset duration "
                                 + toString (Time::fromTimestamp (*stream.durationPts, fromAv (stream.timeBase))));
            }

            target = stream.startPts + *stream.durationPts;
            adjustment = Adjustment::clampedToLast;
        }

        std::int64_t start = windowEdge (target, tolerance.before, stream.timeBase, /*isBefore=*/true);
        std::int64_t end = windowEdge (target, tolerance.after, stream.timeBase, /*isBefore=*/false);
        const bool infiniteBefore = start == std::numeric_limits<std::int64_t>::min();
        // Nearest-keyframe mode is an infinite `before` on a seekable source. Without a usable seek
        // every frame is decoded anyway, so the request falls back to exact selection instead of
        // returning an arbitrary non-keyframe.
        const bool keyframeMode = infiniteBefore && stream.seekable;

        if (infiniteBefore && ! stream.seekable)
        {
            start = target;
            end = target;
        }

        return RequestWindow{ target, start, end, adjustment, keyframeMode };
    }

    // The open-time probe: the first frame of the stream, whatever its time.
    [[nodiscard]] static RequestWindow anyFrameFrom (std::int64_t startPts) noexcept
    {
        return RequestWindow{ startPts, std::numeric_limits<std::int64_t>::min(),
                              std::numeric_limits<std::int64_t>::max(), Adjustment::none, /*keyframeMode=*/false };
    }

    // The same request narrowed to exactly `ts`, recording why: nearest-keyframe mode found every
    // keyframe at or before the target trimmed away by an edit list, so the first presented frame is
    // the answer. The request stays a nearest-keyframe request; only its window changes.
    [[nodiscard]] RequestWindow collapsedTo (std::int64_t ts, Adjustment reason) const noexcept
    {
        return RequestWindow{ ts, ts, ts, reason, keyframeMode };
    }

    [[nodiscard]] std::int64_t getTarget() const noexcept { return target; }
    // Earliest acceptable presentation time; INT64_MIN for an infinite `before` tolerance.
    [[nodiscard]] std::int64_t getStart() const noexcept { return start; }
    // Latest acceptable presentation time; INT64_MAX for an infinite `after` tolerance.
    [[nodiscard]] std::int64_t getEnd() const noexcept { return end; }
    // What the mapping already had to do to the request (a time past the declared duration, clamped).
    [[nodiscard]] Adjustment getAdjustment() const noexcept { return adjustment; }
    // Whether the request asks for the keyframe at or before the target rather than the exact frame.
    [[nodiscard]] bool isKeyframeMode() const noexcept { return keyframeMode; }

private:
    RequestWindow (std::int64_t targetTs, std::int64_t startTs, std::int64_t endTs, Adjustment made,
                   bool nearestKeyframe) noexcept
      : target (targetTs), start (startTs), end (endTs), adjustment (made), keyframeMode (nearestKeyframe)
    {
    }

    // One edge of the tolerance window in stream ticks. A tolerance that reaches beyond half the
    // timestamp domain is treated as an infinite one.
    //
    // The edge is formed first and compared afterwards: `ticks > target - INT64_MIN / 2` is the same
    // inequality, but forms `target - INT64_MIN / 2` even when the tolerance is zero, which overflows
    // for a target past INT64_MAX / 2 -- reachable on a source with no declared duration, where the
    // bounds check cannot reject the request first.
    [[nodiscard]] static std::int64_t windowEdge (std::int64_t target, Time tolerance, AVRational timeBase,
                                                  bool isBefore) noexcept
    {
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();

        if (tolerance.isPositiveInfinity()) return isBefore ? min : max;
        std::int64_t ticks = 0;

        if (tolerance.isFinite() && tolerance > Time::zero())
        {
            ticks = tolerance.toTimestamp (fromAv (timeBase), TimeRounding::down);
        }

        std::int64_t edge = 0;

        if (isBefore)
        {
            if (__builtin_sub_overflow (target, ticks, &edge) || edge < min / 2) return min;
            return edge;
        }

        if (__builtin_add_overflow (target, ticks, &edge) || edge > max / 2) return max;
        return edge;
    }

    std::int64_t target;
    std::int64_t start;
    std::int64_t end;
    Adjustment adjustment;
    bool keyframeMode;
};

} // namespace stills::detail

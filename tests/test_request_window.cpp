// RequestWindow maps a requested Time into the stream's timestamp domain and applies the bounds
// policy and the tolerance. Pure arithmetic over StreamInfo, so these tests need no media file.
#include <limits>

#include <stills/detail/stills_RequestWindow.h>

#include "support/stills_TestCommon.h"

using stills::Adjustment;
using stills::ErrorCode;
using stills::OutOfRangePolicy;
using stills::Time;
using stills::Tolerance;
using stills::detail::RequestWindow;
using stills::detail::StreamInfo;

namespace
{

// 30 fps in a 1/30 s time base, 120 frames (4 s), starting at zero.
StreamInfo thirtyFps()
{
    StreamInfo stream;
    stream.timeBase = AVRational{ 1, 30 };
    stream.startPts = 0;
    stream.durationPts = 120;
    stream.frameDurationHint = 1;
    stream.seekable = true;
    return stream;
}

constexpr auto int64Min = std::numeric_limits<std::int64_t>::min();
constexpr auto int64Max = std::numeric_limits<std::int64_t>::max();

} // namespace

TEST_CASE ("RequestWindow: an exact request is one tick wide", "[window]")
{
    const auto window =
        REQUIRE_OK (RequestWindow::resolve (Time{ 1, 1 }, Tolerance::exact(), thirtyFps(), OutOfRangePolicy::error));
    REQUIRE (window.getTarget() == 30);
    REQUIRE (window.getStart() == 30);
    REQUIRE (window.getEnd() == 30);
    REQUIRE (window.getAdjustment() == Adjustment::none);
    REQUIRE_FALSE (window.isKeyframeMode());
}

TEST_CASE ("RequestWindow: the time origin is added and rounding is to the nearest tick", "[window]")
{
    StreamInfo stream = thirtyFps();
    stream.startPts = 900; // an MPEG-TS that starts at 30 s
    const auto window =
        REQUIRE_OK (RequestWindow::resolve (Time{ 1, 1 }, Tolerance::exact(), stream, OutOfRangePolicy::error));
    REQUIRE (window.getTarget() == 930);

    // 29/30 s stored in a coarse base must land on frame 29, not 28.
    StreamInfo coarse = thirtyFps();
    coarse.timeBase = AVRational{ 1, 1000 };
    coarse.durationPts = 4000;
    const auto rounded =
        REQUIRE_OK (RequestWindow::resolve (Time{ 29, 30 }, Tolerance::exact(), coarse, OutOfRangePolicy::error));
    REQUIRE (rounded.getTarget() == 967);
}

TEST_CASE ("RequestWindow: invalid times are rejected before anything is mapped", "[window]")
{
    REQUIRE_ERROR (RequestWindow::resolve (Time::invalid(), Tolerance::exact(), thirtyFps(), OutOfRangePolicy::error),
                   ErrorCode::invalidArgument);
    REQUIRE_ERROR (
        RequestWindow::resolve (Time::positiveInfinity(), Tolerance::exact(), thirtyFps(), OutOfRangePolicy::error),
        ErrorCode::invalidArgument);
    REQUIRE_ERROR (RequestWindow::resolve (Time{ -1, 30 }, Tolerance::exact(), thirtyFps(), OutOfRangePolicy::error),
                   ErrorCode::invalidArgument);
}

TEST_CASE ("RequestWindow: past the duration is an error or a clamp, with one frame of slack", "[window]")
{
    // One frame past the declared duration is left to the decoder: containers understate by a frame.
    const auto slack =
        REQUIRE_OK (RequestWindow::resolve (Time{ 121, 30 }, Tolerance::exact(), thirtyFps(), OutOfRangePolicy::error));
    REQUIRE (slack.getTarget() == 121);
    REQUIRE (slack.getAdjustment() == Adjustment::none);

    REQUIRE_ERROR (RequestWindow::resolve (Time{ 122, 30 }, Tolerance::exact(), thirtyFps(), OutOfRangePolicy::error),
                   ErrorCode::timeOutOfRange);

    const auto clamped = REQUIRE_OK (
        RequestWindow::resolve (Time{ 10, 1 }, Tolerance::exact(), thirtyFps(), OutOfRangePolicy::clampToLastFrame));
    REQUIRE (clamped.getTarget() == 120);
    REQUIRE (clamped.getAdjustment() == Adjustment::clampedToLast);
}

TEST_CASE ("RequestWindow: a time the stream's domain cannot hold is out of range", "[window]")
{
    StreamInfo stream = thirtyFps();
    stream.durationPts.reset();
    stream.startPts = int64Max - 10;
    REQUIRE_ERROR (RequestWindow::resolve (Time{ 1, 1 }, Tolerance::exact(), stream, OutOfRangePolicy::error),
                   ErrorCode::timeOutOfRange);
}

TEST_CASE ("RequestWindow: finite tolerances widen the window, infinite ones saturate it", "[window]")
{
    const Tolerance half{ Time{ 1, 2 }, Time{ 1, 4 } };
    const auto window = REQUIRE_OK (RequestWindow::resolve (Time{ 2, 1 }, half, thirtyFps(), OutOfRangePolicy::error));
    REQUIRE (window.getTarget() == 60);
    REQUIRE (window.getStart() == 45);
    REQUIRE (window.getEnd() == 67); // 7.5 ticks rounded down
    REQUIRE_FALSE (window.isKeyframeMode());

    const Tolerance afterOnly{ Time::zero(), Time::positiveInfinity() };
    const auto open =
        REQUIRE_OK (RequestWindow::resolve (Time{ 2, 1 }, afterOnly, thirtyFps(), OutOfRangePolicy::error));
    REQUIRE (open.getStart() == 60);
    REQUIRE (open.getEnd() == int64Max);
    REQUIRE_FALSE (open.isKeyframeMode());
}

TEST_CASE ("RequestWindow: an infinite `before` is nearest-keyframe mode only on a seekable source", "[window]")
{
    const auto seekable =
        REQUIRE_OK (RequestWindow::resolve (Time{ 2, 1 }, Tolerance::any(), thirtyFps(), OutOfRangePolicy::error));
    REQUIRE (seekable.isKeyframeMode());
    REQUIRE (seekable.getStart() == int64Min);
    REQUIRE (seekable.getEnd() == int64Max);

    // Without a usable seek every frame is decoded anyway: the request collapses to exact selection.
    StreamInfo pipe = thirtyFps();
    pipe.seekable = false;
    const auto exact =
        REQUIRE_OK (RequestWindow::resolve (Time{ 2, 1 }, Tolerance::any(), pipe, OutOfRangePolicy::error));
    REQUIRE_FALSE (exact.isKeyframeMode());
    REQUIRE (exact.getStart() == 60);
    REQUIRE (exact.getEnd() == 60);
}

TEST_CASE ("RequestWindow: a tolerance past half the domain is treated as infinite", "[window]")
{
    StreamInfo stream = thirtyFps();
    stream.durationPts.reset();
    stream.startPts = int64Max / 2 + 100;
    const auto window = REQUIRE_OK (
        RequestWindow::resolve (Time::zero(), Tolerance::symmetric (Time{ 1, 1 }), stream, OutOfRangePolicy::error));
    REQUIRE (window.getEnd() == int64Max);
    REQUIRE (window.getStart() == stream.startPts - 30);
}

TEST_CASE ("RequestWindow: collapsing keeps the mode and records the reason", "[window]")
{
    const auto window =
        REQUIRE_OK (RequestWindow::resolve (Time{ 2, 1 }, Tolerance::any(), thirtyFps(), OutOfRangePolicy::error));
    const RequestWindow collapsed = window.collapsedTo (3, Adjustment::keyframeBeforeEdit);
    REQUIRE (collapsed.getTarget() == 3);
    REQUIRE (collapsed.getStart() == 3);
    REQUIRE (collapsed.getEnd() == 3);
    REQUIRE (collapsed.getAdjustment() == Adjustment::keyframeBeforeEdit);
    REQUIRE (collapsed.isKeyframeMode());

    const RequestWindow probe = RequestWindow::anyFrameFrom (900);
    REQUIRE (probe.getTarget() == 900);
    REQUIRE (probe.getStart() == int64Min);
    REQUIRE (probe.getEnd() == int64Max);
    REQUIRE_FALSE (probe.isKeyframeMode());
}

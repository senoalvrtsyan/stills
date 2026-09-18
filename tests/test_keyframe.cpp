#include "support/stills_TestCommon.h"

using namespace testsupport;
using stills::Time;
using stills::Tolerance;

TEST_CASE ("tolerance: any() returns the keyframe at or before the time", "[sync][keyframe]")
{
    stills::Options o = swOptions();
    o.tolerance = Tolerance::any();
    auto g = openCounter (o);

    for (int n : { 0, 15, 29, 30, 45, 89, 119 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        const int expected = (n / 30) * 30;
        INFO ("request " << n);
        CHECK (frameIndexOf (img) == expected);
        CHECK (img.getActualTime() == Time{ expected, 30 });
        CHECK (img.isKeyframe());
    }
}

TEST_CASE ("tolerance: a finite window stops decoding at the first acceptable frame", "[sync][keyframe]")
{
    SECTION ("before-tolerance covering the keyframe")
    {
        stills::Options o = swOptions();
        o.tolerance = Tolerance{ Time{ 1, 1 }, Time::zero() }; // up to 1 s early is fine
        auto g = openCounter (o);
        auto img = REQUIRE_OK (g.imageAt (Time{ 47, 30 }));
        CHECK (frameIndexOf (img) == 30); // keyframe 30 is within 1 s before 47 and comes first
        CHECK (img.isKeyframe());
        img = REQUIRE_OK (g.imageAt (Time{ 100, 30 }));
        CHECK (frameIndexOf (img) == 90);
    }

    SECTION ("before-tolerance too small to reach the keyframe")
    {
        stills::Options o = swOptions();
        o.tolerance = Tolerance{ Time{ 5, 30 }, Time::zero() }; // 5 frames early is fine
        auto g = openCounter (o);
        auto img = REQUIRE_OK (g.imageAt (Time{ 47, 30 }));
        CHECK (frameIndexOf (img) == 42); // first decoded frame inside [42, 47]
    }

    SECTION ("after-tolerance never replaces an already decoded exact frame")
    {
        stills::Options o = swOptions();
        o.tolerance = Tolerance{ Time::zero(), Time{ 1, 1 } };
        auto g = openCounter (o);
        auto img = REQUIRE_OK (g.imageAt (Time{ 47, 30 }));
        CHECK (frameIndexOf (img) == 47);
    }

    SECTION ("negative tolerances are rejected at open")
    {
        stills::Options o = swOptions();
        o.tolerance = Tolerance{ Time{ -1, 1 }, Time::zero() };
        REQUIRE_ERROR (stills::AssetImageGenerator::open (fixture ("counter.mp4").string(), o),
                       stills::ErrorCode::invalidArgument);
    }

    SECTION ("clampToLastFrame with any() returns the last keyframe, flagged")
    {
        stills::Options o = swOptions();
        o.tolerance = Tolerance::any();
        o.outOfRange = stills::OutOfRangePolicy::clampToLastFrame;
        auto g = openCounter (o);
        auto img = REQUIRE_OK (g.imageAt (Time{ 10, 1 }));
        CHECK (frameIndexOf (img) == 90);
        CHECK (img.isKeyframe());
        CHECK (img.wasClamped());
    }
}

// Keyframe mode on an edit-list source. The keyframe covering the first presented second (source
// frame 30) precedes the edit and is never output, so requests there clamp to the first presented
// frame; the landing must be verified against the index (the mov seek lands one GOP early just
// after keyframe 90, at 57/30 s).
TEST_CASE ("tolerance: any() on an edit-list source clamps to the first presented frame and finds the "
           "last keyframe",
           "[sync][keyframe][editlist]")
{
    stills::Options o = swOptions();
    o.tolerance = Tolerance::any();
    auto g = REQUIRE_OK (stills::AssetImageGenerator::open (fixture ("counter_editlist.mp4").string(), o));
    struct Case
    {
        int n;
        int expected;
        bool clamped;
    };

    // presented: source 33..119 at 0..86/30 s; keyframes 60 at 27/30 s and 90 at 57/30 s
    const Case cases[] = { { 0, 33, true },   { 10, 33, true },  { 26, 33, true },  { 27, 60, false },
                           { 28, 60, false }, { 40, 60, false }, { 56, 60, false }, { 57, 90, false },
                           { 58, 90, false }, { 86, 90, false }, { 20, 33, true },  { 57, 90, false },
                           { 5, 33, true },   { 59, 90, false }, { 27, 60, false }, { 0, 33, true } };

    for (const Case& c : cases)
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ c.n, 30 }));
        INFO ("t = " << c.n << "/30: got " << frameIndexOf (img) << " at " << img.getActualTime() << " key "
                     << img.isKeyframe() << " clamped " << img.wasClamped());
        CHECK (frameIndexOf (img) == c.expected);
        CHECK (img.wasClamped() == c.clamped);

        if (! c.clamped) CHECK (img.isKeyframe());
        CHECK (img.getActualTime() == Time{ c.expected - 33, 30 });
    }
}

// Nearest-keyframe mode answers with the keyframe at or before the request without decoding
// anything after it, so on a file whose data stops earlier than its container claims it used to
// hand back the last keyframe unflagged, six seconds past the end, where exact mode reports
// timeOutOfRange. The tail is verified by reading packets once per source.
TEST_CASE ("keyframe: past the end of a truncated file the out-of-range policy applies", "[keyframe][bounds]")
{
    const auto path = fixture ("counter_trunc.mp4").string();
    stills::Options any = swOptions();
    any.tolerance = stills::Tolerance::any();
    SECTION ("error policy")
    {
        auto g = REQUIRE_OK (stills::AssetImageGenerator::open (path, any));
        // Somewhere inside the data the keyframe answer is still the keyframe answer.
        auto early = REQUIRE_OK (g.imageAt (Time{ 10, 30 }));
        CHECK (early.isKeyframe());
        CHECK_FALSE (early.wasClamped());
        // Past the data, twice: the second must answer like the first.
        auto first = g.imageAt (Time{ 115, 30 });
        INFO ("first: " << (first ? std::string{ "image" } : toString (first.error())));
        REQUIRE_FALSE (first.has_value());
        CHECK (first.error().code == stills::ErrorCode::timeOutOfRange);
        auto again = g.imageAt (Time{ 115, 30 });
        REQUIRE_FALSE (again.has_value());
        CHECK (again.error().code == stills::ErrorCode::timeOutOfRange);
        // Exact mode agrees, which is the point: the two modes must not disagree about the end.
        REQUIRE_ERROR (g.imageAt (Time{ 115, 30 }, stills::RequestOptions{ .tolerance = stills::Tolerance::exact() }),
                       stills::ErrorCode::timeOutOfRange);
    }

    SECTION ("clamp policy flags the frame")
    {
        stills::Options clamp = any;
        clamp.outOfRange = stills::OutOfRangePolicy::clampToLastFrame;
        auto g = REQUIRE_OK (stills::AssetImageGenerator::open (path, clamp));
        auto img = REQUIRE_OK (g.imageAt (Time{ 115, 30 }));
        CHECK (img.wasClamped());
    }
}

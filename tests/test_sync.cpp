#include <algorithm>
#include <random>

#include "support/stills_TestCommon.h"

using namespace testsupport;
using namespace std::chrono_literals;
using stills::ErrorCode;
using stills::PixelFormat;
using stills::Time;

TEST_CASE ("sync: frame-accurate extraction at exact frame times", "[sync][accuracy]")
{
    const PixelFormat fmt = GENERATE (PixelFormat::yuv420p, PixelFormat::rgba);
    auto g = openCounter (swOptions (fmt));

    for (int n : { 0, 1, 15, 29, 30, 31, 59, 60, 89, 90, 91, 118, 119 })
    {
        INFO ("frame " << n << " format " << fmt);
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        CHECK (frameIndexOf (img) == n);
        CHECK (img.getActualTime() == Time{ n, 30 });
        CHECK (img.isKeyframe() == (n % 30 == 0));
        CHECK_FALSE (img.wasClamped());
        CHECK_FALSE (img.isCorrupt());
    }
}

TEST_CASE ("sync: times between frames resolve to the frame being displayed", "[sync][accuracy]")
{
    auto g = openCounter();
    SECTION ("midpoints")
    {
        for (int n : { 0, 14, 29, 30, 77, 119 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ 2 * n + 1, 60 }));
            CHECK (frameIndexOf (img) == n);
            CHECK (img.getActualTime() == Time{ n, 30 });
        }
    }

    SECTION ("requests round to the nearest stream tick")
    {
        // One tick of counter.mp4 is 1/15360 s (65 us). More than half a tick before a boundary is
        // the previous frame; within half a tick it is the frame at the boundary (see README).
        for (int n : { 1, 30, 60, 119 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 } - Time{ 1, 15360 })); // one full tick early
            CHECK (frameIndexOf (img) == n - 1);
            img = REQUIRE_OK (g.imageAt (Time{ n, 30 } - 1us)); // a quarter of a tick early
            CHECK (frameIndexOf (img) == n);
            img = REQUIRE_OK (g.imageAt (Time{ n, 30 } + 1us));
            CHECK (frameIndexOf (img) == n);
        }
    }

    SECTION ("chrono literal requests")
    {
        auto img = REQUIRE_OK (g.imageAt (1500ms));
        CHECK (frameIndexOf (img) == 45);
        img = REQUIRE_OK (g.imageAt (Time::seconds (2.0)));
        CHECK (frameIndexOf (img) == 60);
    }
}

TEST_CASE ("sync: bounds", "[sync][bounds]")
{
    SECTION ("beyond the last frame is an error by default")
    {
        auto g = openCounter();
        REQUIRE_ERROR (g.imageAt (Time{ 4, 1 }), ErrorCode::timeOutOfRange);
        REQUIRE_ERROR (g.imageAt (Time{ 10, 1 }), ErrorCode::timeOutOfRange);
        auto img = REQUIRE_OK (g.imageAt (Time{ 4, 1 } - Time{ 1, 60 }));
        CHECK (frameIndexOf (img) == 119);
        CHECK_FALSE (img.wasClamped());
    }

    SECTION ("clamp policy returns the last frame, flagged")
    {
        stills::Options o = swOptions();
        o.outOfRange = stills::OutOfRangePolicy::clampToLastFrame;
        auto g = openCounter (o);
        auto img = REQUIRE_OK (g.imageAt (Time{ 10, 1 }));
        CHECK (frameIndexOf (img) == 119);
        CHECK (img.getActualTime() == Time{ 119, 30 });
        CHECK (img.wasClamped());
        img = REQUIRE_OK (g.imageAt (Time{ 4, 1 }));
        CHECK (frameIndexOf (img) == 119);
        CHECK (img.wasClamped());
    }

    SECTION ("negative and non-finite times are rejected")
    {
        auto g = openCounter();
        REQUIRE_ERROR (g.imageAt (Time{ -1, 30 }), ErrorCode::invalidArgument);
        REQUIRE_ERROR (g.imageAt (Time::invalid()), ErrorCode::invalidArgument);
        REQUIRE_ERROR (g.imageAt (Time::positiveInfinity()), ErrorCode::invalidArgument);
        auto img = REQUIRE_OK (g.imageAt (Time::zero())); // still usable afterwards
        CHECK (frameIndexOf (img) == 0);
    }
}

TEST_CASE ("sync: request order does not affect results", "[sync][accuracy]")
{
    auto g = openCounter();
    SECTION ("monotonic sweep uses the forward fast path")
    {
        for (int n = 0; n < 120; ++n)
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
            REQUIRE (frameIndexOf (img) == n);
        }
    }

    SECTION ("random permutation")
    {
        std::vector<int> order (120);
        std::iota (order.begin(), order.end(), 0);
        std::shuffle (order.begin(), order.end(), std::mt19937{ 42 });

        for (int n : order)
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
            REQUIRE (frameIndexOf (img) == n);
        }
    }

    SECTION ("GOP boundary forward and backward")
    {
        for (int n : { 29, 30, 31, 31, 30, 29, 60, 59, 61 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
            REQUIRE (frameIndexOf (img) == n);
        }
    }

    SECTION ("repeated identical requests")
    {
        for (int i = 0; i < 3; ++i)
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ 47, 30 }));
            REQUIRE (frameIndexOf (img) == 47);
        }
    }
}

TEST_CASE ("sync: variable frame rate uses the frame's own interval", "[sync][vfr]")
{
    auto g = REQUIRE_OK (stills::AssetImageGenerator::open (fixture ("counter_vfr.mp4").string(), swOptions()));

    // Every third source frame survives: 0, 3, 6, ...; a request inside a gap gets the frame on
    // screen.
    for (int n : { 0, 1, 2, 3, 29, 30, 31, 100 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        const int expected = (n / 3) * 3;
        CHECK (frameIndexOf (img) == expected);
        CHECK (img.getActualTime() == Time{ expected, 30 });
    }
}

#if __has_include(<unistd.h>)
#include <fcntl.h>
#include <unistd.h>
#define STILLS_HAVE_POSIX_FD 1
#endif

TEST_CASE ("sync: raw elementary stream has no timestamps or index", "[sync][raw]")
{
    auto g = REQUIRE_OK (stills::AssetImageGenerator::open (fixture ("counter.h264").string(), swOptions()));
    CHECK (g.getInfo().containerName == "h264");
    CHECK_FALSE (g.getInfo().duration.has_value());
    // The container has no timestamps; the pipeline stamps frames in display order using the
    // codec-level frame rate (SPS timing), so requests are exact and time zero is the first frame.
    CHECK (g.getInfo().averageFrameRate == stills::Rational{ 30, 1 });

    for (int k : { 0, 5, 40, 41, 100, 30, 3, 90, 119 })
    { // 30 and 3 go backwards: forces a re-open
        auto img = REQUIRE_OK (g.imageAt (Time{ k, 30 }));
        INFO ("request " << k);
        CHECK (frameIndexOf (img) == k);
        CHECK (img.getActualTime() == Time{ k, 30 });
    }

    REQUIRE_ERROR (g.imageAt (Time{ 120, 30 }),
                   ErrorCode::timeOutOfRange); // decided at EOF (no duration)
}

#if STILLS_HAVE_POSIX_FD
TEST_CASE ("sync: non-seekable pipe input supports forward requests only", "[sync][pipe]")
{
    const int fd = ::open (fixture ("counter.mp4").c_str(), O_RDONLY);
    REQUIRE (fd >= 0);
    {
        auto g = REQUIRE_OK (stills::AssetImageGenerator::open ("pipe:" + std::to_string (fd), swOptions()));

        for (int k : { 0, 10, 50 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ k, 30 }));
            CHECK (frameIndexOf (img) == k);
        }

        REQUIRE_ERROR (g.imageAt (Time{ 20, 30 }), ErrorCode::notSeekable); // cannot rewind a pipe
        auto img = REQUIRE_OK (g.imageAt (Time{ 60, 30 }));                 // still usable going forward
        CHECK (frameIndexOf (img) == 60);
        REQUIRE_ERROR (g.imageAt (Time{ 7, 30 }), ErrorCode::notSeekable);
    }

    ::close (fd);
    SECTION ("Tolerance::any() falls back to exact selection without a usable seek")
    {
        const int fd2 = ::open (fixture ("counter.mp4").c_str(), O_RDONLY);
        REQUIRE (fd2 >= 0);
        stills::Options o = swOptions();
        o.tolerance = stills::Tolerance::any();
        auto g = REQUIRE_OK (stills::AssetImageGenerator::open ("pipe:" + std::to_string (fd2), o));
        auto img = REQUIRE_OK (g.imageAt (Time{ 47, 30 }));
        CHECK (frameIndexOf (img) == 47); // not an arbitrary later frame
        ::close (fd2);
    }
}
#endif

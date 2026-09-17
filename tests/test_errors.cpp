#include <limits>
#include <sstream>

#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::Time;

TEST_CASE ("errors: truncated file", "[errors]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_trunc.mp4").string(), swOptions()));
    auto early = REQUIRE_OK (g.imageAt (Time{ 5, 30 }));
    CHECK (frameIndexOf (early) == 5);
    auto late = g.imageAt (Time{ 110, 30 });
    REQUIRE_FALSE (late.has_value());
    INFO ("late error: " << late.error());
    CHECK ((late.error().code == ErrorCode::timeOutOfRange || late.error().code == ErrorCode::endOfStream
            || late.error().code == ErrorCode::decodeFailed));
    // The generator stays usable after a per-request failure.
    auto again = REQUIRE_OK (g.imageAt (Time{ 7, 30 }));
    CHECK (frameIndexOf (again) == 7);
}

TEST_CASE ("errors: corrupt data never yields a silently wrong frame", "[errors]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_corrupt.mp4").string(), swOptions()));

    for (int n = 0; n < 120; n += 7)
    {
        auto r = g.imageAt (Time{ n, 30 });

        if (! r)
        {
            INFO ("frame " << n << ": " << r.error());
            CHECK ((r.error().code == ErrorCode::decodeFailed || r.error().code == ErrorCode::endOfStream
                    || r.error().code == ErrorCode::timeOutOfRange));
            continue;
        }

        // The content must match the frame the library claims to have returned, unless flagged corrupt.
        const int claimed = frameIndexOfTime (r->getActualTime());
        INFO ("frame " << n << " claimed " << claimed << " content " << frameIndexOf (*r) << " corrupt "
                       << r->isCorrupt());
        CHECK ((frameIndexOf (*r) == claimed || r->isCorrupt()));
        CHECK (r->getActualTime() <= Time{ n, 30 });
    }

    // Frames before the damage are exact.
    for (int n : { 0, 10, 20 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        CHECK (frameIndexOf (img) == n);
        CHECK_FALSE (img.isCorrupt());
    }
}

TEST_CASE ("errors: Error rendering", "[errors]")
{
    const stills::Error e{ ErrorCode::openFailed, AVERROR (EACCES), "avformat_open_input(\"x\"): Permission denied" };
    const std::string s = toString (e);
    CHECK (s.find ("openFailed") != std::string::npos);
    CHECK (s.find ("Permission denied") != std::string::npos);
    CHECK (s.find ("AVERROR") != std::string::npos);
    CHECK (toString (stills::Error{ ErrorCode::cancelled }) == "cancelled");
    std::ostringstream os;
    os << ErrorCode::noVideoStream;
    CHECK (os.str() == "noVideoStream");
#if defined(__cpp_lib_format)
    CHECK (std::format ("{}", ErrorCode::decodeFailed) == "decodeFailed");
#endif
}

// Two signed additions in the request path could overflow: the requested time plus the stream's
// time origin (an MPEG-TS starting at 10 s), and the keyframe-mode scan horizon, which is handed
// INT64_MAX for "the next keyframe, wherever it is". Under GCC's wrap both failed as intended;
// under a consumer's UBSan, or this project's asan preset (-fno-sanitize-recover=all), the process
// aborted instead of returning an error. Both are checked here, so the asan preset covers them.
TEST_CASE ("errors: huge times on an offset stream do not overflow", "[errors][overflow]")
{
    auto g = REQUIRE_OK (stills::AssetImageGenerator::open (fixture ("counter_offset.ts").string(), swOptions()));
    const Time huge[] = {
        Time{ std::numeric_limits<std::int64_t>::max(), 1 },
        Time{ std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int32_t>::max() },
        Time{ std::numeric_limits<std::int64_t>::max() / 2, 1 },
        Time{ std::numeric_limits<std::int64_t>::max() / 90000, 1 },
    };

    for (const Time t : huge)
    {
        auto img = g.imageAt (t);
        INFO ("exact " << t);
        REQUIRE_FALSE (img.has_value());
        CHECK ((img.error().code == stills::ErrorCode::timeOutOfRange
                || img.error().code == stills::ErrorCode::invalidArgument));
    }

    // Keyframe mode, after a byte seek has positioned the scanned index: the horizon is INT64_MAX.
    stills::RequestOptions any;
    any.tolerance = stills::Tolerance::any();
    (void)REQUIRE_OK (g.imageAt (Time{ 90, 30 }, any));
    (void)REQUIRE_OK (g.imageAt (Time{ 40, 30 }, any));
    (void)REQUIRE_OK (g.imageAt (Time{ 95, 30 }, any));

    for (const Time t : huge)
    {
        auto img = g.imageAt (t, any);
        INFO ("keyframe " << t);
        REQUIRE_FALSE (img.has_value());
    }

    // The generator is still usable afterwards.
    auto ok = REQUIRE_OK (g.imageAt (Time{ 7, 30 }));
    CHECK (frameIndexOf (ok) == 7);
}

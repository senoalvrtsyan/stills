// A cancellation that lands inside libavformat I/O (the interrupt callback firing during a seek's
// binary search or a read) must not poison the demuxer for later, live requests.
#include <atomic>
#include <latch>
#include <stills/detail/stills_FramePipeline.h>

#include "support/stills_TestCommon.h"

using namespace testsupport;
using namespace std::chrono_literals;
using stills::ErrorCode;
using stills::Time;

TEST_CASE ("cancel: a live request after an interrupted one never reports cancelled", "[async][cancel][ts]")
{
    // FramePipeline-level and deterministic: with a token that is already cancelled, the interrupt
    // callback fires inside the very first libavformat call of the request (the MPEG-TS seek's
    // binary search reads), which leaves AVIOContext at "EOF with error = AVERROR_EXIT".
    // counter_longgop.ts is the fixture B1 failed on: its long GOPs mean a cancelled request
    // has usually only read packets, so the interrupt lands in a read rather than in a seek.
    const char* file = GENERATE ("counter_offset.ts", "counter_longgop.ts", "counter.mkv", "counter.mp4");
    auto p = REQUIRE_OK (stills::detail::FramePipeline::open (fixture (file).string(), swOptions()));
    std::atomic<bool> cancelled{ true };
    const stills::detail::CancelToken dead{ &cancelled, nullptr };
    const stills::detail::CancelToken live{};
    INFO (file);
    auto warm = REQUIRE_OK (p->imageAt (Time{ 5, 30 }, live));
    CHECK (frameIndexOf (warm) == 5);

    for (int round = 0; round < 3; ++round)
    {
        auto r = p->imageAt (Time{ 100, 30 }, dead);
        REQUIRE_FALSE (r.has_value());
        CHECK (r.error().code == ErrorCode::cancelled);
        // B1: the interrupted read leaves libavio at end-of-file, and a seek does not clear it. If
        // recovery is skipped this request reads EOF before a single packet and reports endOfStream.
        auto ok = REQUIRE_OK (p->imageAt (Time{ 100, 30 }, live));
        CHECK (frameIndexOf (ok) == 100);
        auto back = REQUIRE_OK (p->imageAt (Time{ 3, 30 }, live)); // backward: needs a seek to work
        CHECK (frameIndexOf (back) == 3);
        auto fwd = REQUIRE_OK (p->imageAt (Time{ 4, 30 }, live)); // forward fast path still intact
        CHECK (frameIndexOf (fwd) == 4);
    }

    CHECK (p->getInfo().seekable); // the interrupted seeks did not count as seek failures
}

TEST_CASE ("cancel: cancelled batches leave the generator fully usable", "[async][cancel][ts]")
{
    auto g = REQUIRE_OK (stills::AssetImageGenerator::open (fixture ("counter_offset.ts").string(), swOptions()));

    for (int round = 0; round < 5; ++round)
    {
        Collector col;
        std::latch first{ 1 };
        std::atomic<bool> signalled{ false };
        std::vector<Time> times;

        for (int n = 119; n >= 0; n -= 3)
            times.emplace_back (n, 30); // every item seeks backwards
        auto req = g.generateImages (times,
                                     [&] (stills::Completion c)
                                     {
                                         col.add (std::move (c));

                                         if (! signalled.exchange (true)) first.count_down();
                                     });

        first.wait();
        req.cancel();
        REQUIRE (req.waitFor (30s) == stills::WaitResult::finished);
        CHECK (col.count (stills::GenerationStatus::failed) == 0);

        // Live requests afterwards: never `cancelled`, always the right frame.
        for (int n : { 100, 1, 119, 50 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
            CHECK (frameIndexOf (img) == n);
        }
    }
}

#if __has_include(<unistd.h>)
#include <fcntl.h>
#include <unistd.h>
TEST_CASE ("cancel: a cancelled request on a pipe leaves forward requests working", "[cancel][pipe]")
{
    // Non-rewindable input: after a cancelled request the pipeline can only continue forward.
    const int fd = ::open (fixture ("counter.mp4").c_str(), O_RDONLY);
    REQUIRE (fd >= 0);
    {
        auto p = REQUIRE_OK (stills::detail::FramePipeline::open ("pipe:" + std::to_string (fd), swOptions()));
        CHECK_FALSE (p->getInfo().seekable);
        std::atomic<bool> cancelled{ true };
        const stills::detail::CancelToken dead{ &cancelled, nullptr };
        const stills::detail::CancelToken live{};
        auto a = REQUIRE_OK (p->imageAt (Time{ 2, 30 }, live));
        CHECK (frameIndexOf (a) == 2);
        auto r = p->imageAt (Time{ 60, 30 }, dead);
        REQUIRE_FALSE (r.has_value());
        CHECK (r.error().code == ErrorCode::cancelled);
        auto b = REQUIRE_OK (p->imageAt (Time{ 60, 30 }, live));
        CHECK (frameIndexOf (b) == 60);
        REQUIRE_ERROR (p->imageAt (Time{ 1, 30 }, live), ErrorCode::notSeekable);
    }

    ::close (fd);
}
#endif

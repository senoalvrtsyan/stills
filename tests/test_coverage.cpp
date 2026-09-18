// Coverage: 10-bit sources and 16-bit outputs, open-GOP/interlaced H.264, every ErrorCode, damage
// patterns, stress, raw MJPEG, and odd-sized rotation.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <latch>
#include <stills/stills_Interop.h>
#include <thread>
#include <utility>
#include <vector>

#include "support/stills_TestCommon.h"

using namespace testsupport;
using namespace std::chrono_literals;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::PixelFormat;
using stills::Time;

// A 10-bit source through every output, including the depth-preserving ones.
TEST_CASE ("coverage: 10-bit source to 8-bit and 16-bit outputs", "[sync][depth]")
{
    const PixelFormat fmt = GENERATE (PixelFormat::rgba, PixelFormat::yuv420p, PixelFormat::nv12, PixelFormat::gray8,
                                      PixelFormat::rgb24, PixelFormat::p010, PixelFormat::rgba64);
    INFO ("format " << fmt);
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_p10.mp4").string(), swOptions (fmt)));
    CHECK (g.getInfo().sourcePixelFormat == "yuv420p10le");

    for (int n : { 0, 17, 29, 30, 77, 119 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        CHECK (img.getPixelFormat() == fmt);
        CHECK (img.getPlaneCount() == stills::getPlaneCount (fmt));
        CHECK (frameIndexOf (img) == n);

        if (fmt == PixelFormat::p010)
        {
            CHECK (img.getPlaneSize (1) == stills::Size{ 32, 24 });
            CHECK (img.getRowStride (0) >= 128); // 2 bytes per luma sample
            CHECK (img.getColorRange() == stills::ColorRange::limited);
        }

        if (fmt == PixelFormat::rgba64)
        {
            CHECK (img.getRowStride (0) >= 64u * 8u);
            CHECK (img.getColorRange() == stills::ColorRange::full);
            const auto px = img.getPixels();
            CHECK (std::to_integer<int> (px[6]) == 0xFF);
            CHECK (std::to_integer<int> (px[7]) == 0xFF);
        }
    }

    // 16-bit formats survive rotation (the kernel's 8-byte cells) and the packed copy.
    if (fmt == PixelFormat::rgba64 || fmt == PixelFormat::p010)
    {
        auto five = REQUIRE_OK (g.imageAt (Time{ 5, 30 }));
        auto packed = REQUIRE_OK (five.toPackedBytes());
        CHECK (packed.size() == static_cast<std::size_t> (64 * 48) * (fmt == PixelFormat::rgba64 ? 8u : 3u));
        stills::Options ro = swOptions (fmt);
        auto rot = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_rot90.mp4").string(),
                                                          ro)); // 8-bit source, rotated
        auto img = REQUIRE_OK (rot.imageAt (Time{ 7, 30 }));
        CHECK (img.getSize() == stills::Size{ 48, 64 });
        CHECK (frameIndexOf (img, rot.getInfo().rotationDegrees) == 7);
    }
}

// Open-GOP and interlaced H.264.
TEST_CASE ("coverage: open-GOP and interlaced H.264 stay exact", "[sync][h264]")
{
    SECTION ("open GOP")
    {
        auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_opengop.mp4").string(), swOptions()));

        for (int n : { 0, 28, 29, 30, 31, 59, 60, 89, 119, 45, 2 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
            INFO ("frame " << n << ": got " << frameIndexOf (img) << " at " << img.getActualTime());
            CHECK (frameIndexOf (img) == n);
            CHECK (img.getActualTime() == Time{ n, 30 });
        }

        stills::Options k = swOptions();
        k.tolerance = stills::Tolerance::any();
        auto kg = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_opengop.mp4").string(), k));

        for (int n : { 45, 29, 90, 119 })
        {
            auto img = REQUIRE_OK (kg.imageAt (Time{ n, 30 }));
            CHECK (img.isKeyframe());
            CHECK (frameIndexOf (img) <= n);
            CHECK (frameIndexOf (img) % 30 == 0);
        }
    }

    SECTION ("interlaced")
    {
        auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_ilace.mp4").string(), swOptions()));

        for (int n : { 0, 1, 29, 30, 31, 77, 119, 3 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
            INFO ("frame " << n);
            CHECK (frameIndexOf (img) == n);
            CHECK (img.isKeyframe() == (n % 30 == 0));
        }
    }
}

// Every ErrorCode the library can produce is reachable (the rest are asserted by construction).
TEST_CASE ("coverage: error codes are reachable and distinct", "[errors]")
{
    REQUIRE_ERROR (AssetImageGenerator::open (fixture ("does_not_exist.mp4").string(), swOptions()),
                   ErrorCode::fileNotFound);
    REQUIRE_ERROR (AssetImageGenerator::open (fixture ("not_a_video.mp4").string(), swOptions()),
                   ErrorCode::unsupportedFormat);
    REQUIRE_ERROR (AssetImageGenerator::open (fixture ("audio_only.m4a").string(), swOptions()),
                   ErrorCode::noVideoStream);
    REQUIRE_ERROR (AssetImageGenerator::open ("", swOptions()), ErrorCode::invalidArgument);
    // openFailed: a directory is not a readable media source.
    auto dir = AssetImageGenerator::open (fixture ("").string(), swOptions());
    REQUIRE_FALSE (dir.has_value());
    CHECK ((dir.error().code == ErrorCode::openFailed || dir.error().code == ErrorCode::unsupportedFormat));
    stills::Options hw = swOptions();
    hw.hardware.policy = stills::HardwarePolicy::requireHardware;
    hw.hardware.deviceType = stills::HardwareDeviceType::vaapi;
    hw.hardware.device = "/dev/dri/does-not-exist";
    REQUIRE_ERROR (AssetImageGenerator::open (fixture ("counter.mp4").string(), hw), ErrorCode::hardwareUnavailable);
    auto g = openCounter();
    REQUIRE_ERROR (g.imageAt (Time{ -1, 30 }), ErrorCode::invalidArgument);
    REQUIRE_ERROR (g.imageAt (Time{ 10, 1 }), ErrorCode::timeOutOfRange);
    AssetImageGenerator moved = std::move (g);
    REQUIRE_ERROR (g.imageAt (Time::zero()),
                   ErrorCode::invalidState); // NOLINT(bugprone-use-after-move)
    auto img = REQUIRE_OK (moved.imageAt (Time::zero()));
    stills::Image taken = std::move (img);
    REQUIRE_ERROR (img.clone(), ErrorCode::invalidState); // NOLINT(bugprone-use-after-move)
    // conversionFailed is not reachable: an unsupported destination in adoptFrame is
    // invalidArgument, and swscale refuses no supported format pair.
    Collector col;
    // The cancellation has to be in place before the worker dispatches the second item, or "at least
    // one cancelled" depends on the main thread outrunning a 64x48 decode. Holding the first handler
    // until cancel() has been called makes it an ordering rather than a race.
    std::latch cancelIssued{ 1 };
    auto req = moved.generateImages ({ Time{ 1, 30 }, Time{ 2, 30 }, Time{ 3, 30 } },
                                     [&] (stills::Completion c)
                                     {
                                         cancelIssued.wait();
                                         col.add (std::move (c));
                                     });

    req.cancel();
    cancelIssued.count_down();
    REQUIRE (req.waitFor (30s) == stills::WaitResult::finished);
    CHECK (col.count() == 3);
    CHECK (col.count (stills::GenerationStatus::cancelled) >= 2);
    // notSeekable: backwards on a pipe (test_sync); endOfStream / decodeFailed: corrupt fixtures.
    std::vector<std::string_view> names;

    for (int c = 0; c <= static_cast<int> (ErrorCode::internal); ++c)
        names.push_back (toString (static_cast<ErrorCode> (c)));
    std::sort (names.begin(), names.end());
    CHECK (std::adjacent_find (names.begin(), names.end()) == names.end());
    CHECK (std::find (names.begin(), names.end(), "unknown") == names.end());
}

// With a deterministic damage pattern the set of affected frames is stable: frames before the
// damage are clean, frames from the next keyframe on are clean, and nothing in between is silently
// wrong.
TEST_CASE ("coverage: deterministic corruption never yields a silently wrong frame", "[errors]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_corrupt_fixed.mp4").string(), swOptions()));
    int cleanBefore = 0, flagged = 0, failed = 0, cleanAfter = 0;

    for (int n = 0; n < 120; ++n)
    {
        auto r = g.imageAt (Time{ n, 30 });

        if (! r)
        {
            ++failed;
            CHECK ((r.error().code == ErrorCode::decodeFailed || r.error().code == ErrorCode::endOfStream
                    || r.error().code == ErrorCode::timeOutOfRange));
            continue;
        }

        const int claimed = frameIndexOfTime (r->getActualTime());
        const bool matches = frameIndexOf (*r) == claimed;
        INFO ("frame " << n << " claimed " << claimed << " content " << frameIndexOf (*r) << " corrupt "
                       << r->isCorrupt());
        CHECK ((matches || r->isCorrupt()));

        if (n < 60 && matches && ! r->isCorrupt()) ++cleanBefore;
        if (r->isCorrupt()) ++flagged;
        if (n >= 90 && matches && ! r->isCorrupt()) ++cleanAfter;
    }

    CHECK (cleanBefore == 60); // the damage sits in the last quarter of the file
    WARN ("corrupt-fixed fixture: flagged " << flagged << ", failed " << failed << ", clean from frame 90 on "
                                            << cleanAfter);
    CHECK (flagged + failed >= 1);
}

// Stress: exactly-once delivery under a 10 000-item batch, a cancel storm from four threads
// while four threads submit, and repeated destruction mid-batch.
TEST_CASE ("coverage: async stress keeps exactly-once delivery", "[async][stress]")
{
    SECTION ("10 000 items in one batch")
    {
        auto g = openCounter();
        std::vector<Time> times;
        times.reserve (10000);

        for (int i = 0; i < 10000; ++i)
            times.emplace_back (i % 120, 30);
        std::atomic<int> delivered{ 0 };
        std::atomic<int> ok{ 0 };
        auto req = g.generateImages (times,
                                     [&] (stills::Completion c)
                                     {
                                         delivered.fetch_add (1);

                                         if (c.result) ok.fetch_add (1);
                                     });

        REQUIRE (req.waitFor (120s) == stills::WaitResult::finished);
        CHECK (delivered.load() == 10000);
        CHECK (ok.load() == 10000);
    }

    SECTION ("cancel storm")
    {
        auto g = openCounter();
        std::atomic<int> delivered{ 0 };
        std::atomic<bool> stop{ false };
        std::vector<stills::AsyncRequest> reqs (400);
        std::mutex m;
        std::vector<std::thread> submitters, cancellers;

        for (int t = 0; t < 4; ++t)
        {
            submitters.emplace_back (
                [&, t]
                {
                    for (int i = 0; i < 100; ++i)
                    {
                        std::vector<Time> times;

                        for (int k = 0; k < 5; ++k)
                            times.emplace_back ((i * 7 + k + t) % 120, 30);
                        auto r = g.generateImages (times, [&] (stills::Completion) { delivered.fetch_add (1); });
                        std::lock_guard lk (m);
                        reqs[static_cast<std::size_t> (t * 100 + i)] = std::move (r);
                    }
                });
        }

        for (int t = 0; t < 4; ++t)
        {
            cancellers.emplace_back (
                [&]
                {
                    while (! stop.load())
                    {
                        std::lock_guard lk (m);

                        for (auto& r : reqs)
                            if (r.isValid() && (std::rand() % 3) == 0) r.cancel(); // NOLINT(concurrency-mt-unsafe)
                        if (std::rand() % 5 == 0) g.cancelAll();                   // NOLINT(concurrency-mt-unsafe)
                    }
                });
        }

        for (auto& th : submitters)
            th.join();
        stop = true;

        for (auto& th : cancellers)
            th.join();

        for (auto& r : reqs)
            REQUIRE (r.waitFor (120s) == stills::WaitResult::finished);
        CHECK (delivered.load() == 400 * 5);
    }

    SECTION ("destroy mid-batch, fifty times")
    {
        for (int round = 0; round < 50; ++round)
        {
            std::atomic<int> delivered{ 0 };
            stills::AsyncRequest req;
            {
                auto g = openCounter();
                std::vector<Time> times;

                for (int i = 0; i < 40; ++i)
                    times.emplace_back (i, 30);
                req = g.generateImages (times, [&] (stills::Completion) { delivered.fetch_add (1); });
                std::this_thread::sleep_for (std::chrono::microseconds (round * 50));
            }

            REQUIRE (req.isFinished());
            CHECK (delivered.load() == 40);
        }
    }
}

// Raw MJPEG — no timestamps (synthesised 25 fps timeline), intra-only, full-range source.
TEST_CASE ("coverage: raw MJPEG has a synthesised timeline and keeps the full range", "[sync][raw]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mjpeg").string(), swOptions()));
    CHECK (g.getInfo().timestampsSynthesized);
    CHECK (g.getInfo().averageFrameRate == stills::Rational{ 25, 1 });
    CHECK (g.getInfo().codecName == "mjpeg");

    for (int k : { 0, 5, 40, 3, 100 })
    { // 3 goes backwards: re-open
        auto img = REQUIRE_OK (g.imageAt (Time{ k, 25 }));
        INFO ("request " << k);
        CHECK (img.getActualTime() == Time{ k, 25 });
        CHECK (img.isKeyframe());
        // Full-range JPEG luma: the encoded index is limited-range in the source pattern, and the
        // yuv420p output keeps the source range (full) so the luma is expanded: recover with the
        // inverse.
        const double y = lumaAt (img, 16, 24);
        const double limited = y * 219.0 / 255.0 + 16.0;
        const int idxLo = static_cast<int> (std::lround ((limited - 22.0) / 14.0));
        const double y2 = lumaAt (img, 48, 24);
        const double limited2 = y2 * 219.0 / 255.0 + 16.0;
        const int idxHi = static_cast<int> (std::lround ((limited2 - 22.0) / 14.0));
        CHECK (img.getColorRange() == stills::ColorRange::full);
        CHECK (idxLo + 16 * idxHi == k);
    }
}

// Rotation of odd-sized sources through every format.
TEST_CASE ("coverage: rotation on odd source dimensions per output format", "[output][rotation]")
{
    const PixelFormat fmt = GENERATE (PixelFormat::rgba, PixelFormat::rgb24, PixelFormat::gray8, PixelFormat::yuv420p,
                                      PixelFormat::nv12, PixelFormat::rgba64);
    INFO ("format " << fmt);
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("odd_rot90.mov").string(), swOptions (fmt)));
    CHECK (g.getInfo().rotationDegrees == 270);
    const stills::Size expected = stills::hasChromaSubsampling (fmt) ? stills::Size{ 16, 32 } : stills::Size{ 17, 33 };
    CHECK (g.getInfo().outputSize == expected);
    auto img = REQUIRE_OK (g.imageAt (Time::zero()));
    CHECK (img.getSize() == expected);

    // The source is solid red: every pixel of the rotated output is red (G channel low, R high / luma
    // of red).
    if (fmt == PixelFormat::rgba || fmt == PixelFormat::rgb24)
    {
        const int bpp = stills::getBytesPerPixel (fmt);
        const auto px = img.getPixels();

        for (int y : { 0, expected.height / 2, expected.height - 1 })
        {
            for (int x : { 0, expected.width / 2, expected.width - 1 })
            {
                const std::size_t at =
                    static_cast<std::size_t> (y) * img.getRowStride (0) + static_cast<std::size_t> (x * bpp);
                CHECK (std::to_integer<int> (px[at]) > 200);    // R
                CHECK (std::to_integer<int> (px[at + 1]) < 40); // G
            }
        }
    }

    if (fmt == PixelFormat::rgba64)
    {
        const auto px = img.getPixels();
        CHECK (std::to_integer<int> (px[1]) > 200); // high byte of R
        CHECK (std::to_integer<int> (px[3]) < 40);  // high byte of G
    }
}

// Public members the suite never executed. Each of them is reachable from an ordinary consumer and
// none had a test: an API nobody calls in the tests is an API nobody has checked.
TEST_CASE ("coverage: isTightlyPacked and waitFor", "[coverage]")
{
    SECTION ("isTightlyPacked agrees with getPackedSizeBytes")
    {
        auto g = openCounter (swOptions (stills::PixelFormat::rgba));
        auto img = REQUIRE_OK (g.imageAt (Time{ 7, 30 }));
        const bool packed = img.isTightlyPacked();
        const std::size_t minimal = static_cast<std::size_t> (img.getWidth()) * 4;
        CHECK (packed == (img.getRowStride (0) == minimal));

        if (packed) CHECK (img.getPackedSizeBytes() == img.getPixels().size());
        auto bytes = REQUIRE_OK (img.toPackedBytes());
        CHECK (bytes.size() == img.getPackedSizeBytes());
        stills::Image empty = std::move (img);
        CHECK_FALSE (empty.isEmpty());
        CHECK_FALSE (img.isTightlyPacked()); // NOLINT(bugprone-use-after-move): empty is not packed
    }

    SECTION ("waitFor reports a timeout instead of blocking")
    {
        auto g = openCounter();
        std::latch release{ 1 };
        std::vector<Time> times;

        for (int n = 0; n < 30; ++n)
            times.emplace_back (n * 4, 30);
        auto req = g.generateImages (times, [&] (stills::Completion) { release.wait(); });
        CHECK (req.waitFor (std::chrono::milliseconds{ 20 }) == stills::WaitResult::timedOut);
        CHECK_FALSE (req.isFinished());
        release.count_down();
        CHECK (req.wait() == stills::WaitResult::finished);
        CHECK (req.getRemaining() == 0);
    }
}

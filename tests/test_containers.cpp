// Frame accuracy on containers other than MP4: MPEG-TS (seeks land on arbitrary packets),
// Matroska/WebM (millisecond time base), edit lists, and a video stream that is not stream 0.
#include <algorithm>
#include <random>
#include <string_view>

#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::Time;

namespace
{
void expectExact (AssetImageGenerator& g, int n)
{
    INFO ("request " << n);
    auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
    CHECK (frameIndexOf (img) == n);
    CHECK_FALSE (img.wasClamped());
    CHECK_FALSE (img.isCorrupt());
}
} // namespace

TEST_CASE ("containers: MPEG-TS backward and cross-GOP requests are exact", "[sync][ts]")
{
    const stills::HardwarePolicy policy =
        GENERATE (stills::HardwarePolicy::softwareOnly, stills::HardwarePolicy::preferHardware);
    stills::Options o = swOptions();
    o.hardware.policy = policy;
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_offset.ts").string(), o));
    INFO ("decoder: " << (g.getActiveDecoder().hardware ? "hardware" : "software"));
    CHECK (g.getInfo().containerName == "mpegts");
    CHECK (g.getInfo().seekable);
    SECTION ("into the first GOP after moving past it")
    {
        for (int n : { 45, 1, 0, 29, 2, 5 })
            expectExact (g, n);
    }

    SECTION ("into the last GOP, where no keyframe follows the landing point")
    {
        for (int n : { 100, 119, 118, 90, 89, 115 })
            expectExact (g, n);
    }

    SECTION ("random permutation")
    {
        std::vector<int> order (120);
        std::iota (order.begin(), order.end(), 0);
        std::shuffle (order.begin(), order.end(), std::mt19937{ 7 });

        for (int n : order)
            expectExact (g, n);
    }

    SECTION ("backward one frame at a time")
    {
        for (int n = 119; n >= 80; --n)
            expectExact (g, n);
    }

    SECTION ("times before the first frame clamp to it, not to the next keyframe")
    {
        auto late = REQUIRE_OK (g.imageAt (Time{ 70, 30 }));
        CHECK (frameIndexOf (late) == 70);
        auto first = REQUIRE_OK (g.imageAt (Time::zero()));
        CHECK (frameIndexOf (first) == 0);
        CHECK (first.getActualTime() == Time::zero());
        CHECK_FALSE (first.wasClamped());
    }

    SECTION ("keyframe mode returns the keyframe at or before the time")
    {
        stills::Options k = o;
        k.tolerance = stills::Tolerance::any();
        auto kg = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_offset.ts").string(), k));

        for (int n : { 45, 15, 119, 0, 89, 90 })
        {
            auto img = REQUIRE_OK (kg.imageAt (Time{ n, 30 }));
            INFO ("request " << n);
            CHECK (frameIndexOf (img) == (n / 30) * 30);
            CHECK (img.isKeyframe());
        }
    }
}

TEST_CASE ("containers: MPEG-TS with a single GOP needs the explicit start seek", "[sync][ts]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_longgop.ts").string(), swOptions()));

    for (int n : { 45, 1, 100, 119, 60, 0, 31, 30, 89 })
        expectExact (g, n);
    stills::Options o = swOptions();
    o.tolerance = stills::Tolerance::any();
    auto k = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_longgop.ts").string(), o));
    auto img = REQUIRE_OK (k.imageAt (Time{ 100, 30 }));
    CHECK (frameIndexOf (img) == 0); // the only keyframe
    CHECK (img.isKeyframe());
}

TEST_CASE ("containers: Matroska exact frame times return frame k", "[sync][mkv]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mkv").string(), swOptions()));
    CHECK (g.getInfo().containerName.find ("matroska") != std::string::npos);
    CHECK (g.getInfo().timeBase == stills::Rational{ 1, 1000 });

    for (int n : { 29, 5, 2, 119, 89, 0, 1, 30, 31, 60, 118, 45 })
    {
        INFO ("request " << n);
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        CHECK (frameIndexOf (img) == n);
        // getActualTime() is the container's millisecond-rounded value: within half a tick of k/30.
        const double diff = std::abs (img.getActualTime().toSeconds() - n / 30.0);
        CHECK (diff <= 0.0005 + 1e-9);
        CHECK_FALSE (img.wasClamped());
    }

    // Midpoints still resolve to the earlier frame.
    for (int n : { 0, 14, 29, 77 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ 2 * n + 1, 60 }));
        CHECK (frameIndexOf (img) == n);
    }

    // Time::frames() is the canonical way to ask for frame k.
    auto f = REQUIRE_OK (g.imageAt (Time::frames (59, { 30, 1 })));
    CHECK (frameIndexOf (f) == 59);
}

TEST_CASE ("containers: WebM/VP9 exact frame times return frame k", "[sync][mkv]")
{
#if ! STILLS_HAVE_VP9
    SKIP ("ffmpeg was built without libvpx-vp9; fixture not generated");
#else
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_vp9.webm").string(), swOptions()));
    CHECK (g.getInfo().codecName.find ("vp9") != std::string::npos);

    for (int n : { 119, 29, 5, 2, 89, 0, 60, 45 })
    {
        INFO ("request " << n);
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        CHECK (frameIndexOf (img) == n);
    }
#endif
}

TEST_CASE ("containers: edit list — time zero is the first presented frame", "[sync][editlist]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_editlist.mp4").string(), swOptions()));
    auto first = REQUIRE_OK (g.imageAt (Time::zero()));
    const int f0 = frameIndexOf (first);
    CHECK (first.getActualTime() == Time::zero());
    CHECK (f0 >= 30); // the copy started at 1.1 s: the keyframe at 1.0 s is trimmed by the edit list
    CHECK_FALSE (first.isKeyframe());

    for (int k : { 1, 10, 50, 3, 80 })
    {
        INFO ("request " << k);
        auto img = REQUIRE_OK (g.imageAt (Time{ k, 30 }));
        CHECK (frameIndexOf (img) == f0 + k);
        CHECK (img.getActualTime() == Time{ k, 30 });
    }
}

TEST_CASE ("containers: the video stream is not stream 0", "[open]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("av_audio_first.mp4").string(), swOptions()));
    CHECK (g.getInfo().videoStreamIndex == 1);

    for (int n : { 0, 20, 7 })
        expectExact (g, n); // -shortest: one second of video
    stills::Options o = swOptions();
    o.videoStreamIndex = 0; // the audio stream
    REQUIRE_ERROR (AssetImageGenerator::open (fixture ("av_audio_first.mp4").string(), o), ErrorCode::invalidArgument);
    o.videoStreamIndex = 1;
    auto explicitVideo = REQUIRE_OK (AssetImageGenerator::open (fixture ("av_audio_first.mp4").string(), o));
    expectExact (explicitVideo, 12);
}

TEST_CASE ("containers: sync results on every container match the MP4 reference", "[sync][accuracy]")
{
    const char* file = GENERATE ("counter_offset.ts", "counter.mkv", "counter_longgop.ts");
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture (file).string(), swOptions (stills::PixelFormat::rgba)));
    auto ref = openCounter (swOptions (stills::PixelFormat::rgba));

    for (int n : { 3, 90, 31, 119, 0, 64 })
    {
        INFO (file << " frame " << n);
        auto a = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        auto b = REQUIRE_OK (ref.imageAt (Time{ n, 30 }));
        CHECK (frameIndexOf (a) == n);
        CHECK (a.getSize() == b.getSize());

        if (std::string_view{ file } != "counter_longgop.ts") CHECK (a.isKeyframe() == b.isKeyframe());
    }
}

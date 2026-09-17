// Positioning across the containers where a seek does not land where it was aimed: a fragmented
// MP4 whose demuxer skips the reorder shift, open-GOP HEVC leading pictures, keyframe mode holding
// a keyframe, and consecutive pulls across a keyframe boundary. Every case is a sequence of
// requests on one generator, and the assertion is always the same: each request returns exactly
// the frame that covers it, reported at exactly its own time.
#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::Time;

// Pulling consecutive frames across a keyframe. The forward-or-seek decision once compared
// DTS-domain index entries with a PTS: two frames before every keyframe it re-decoded the GOP,
// and the frame it then selected out of that re-decode was the wrong one.
TEST_CASE ("realtime: consecutive pulls across a keyframe stay exact", "[realtime][sync]")
{
    auto g = openCounter();
    auto first = REQUIRE_OK (g.imageAt (Time{ 26, 30 }));
    CHECK (frameIndexOf (first) == 26);
    CHECK (first.getActualTime() == Time{ 26, 30 });

    for (int n = 27; n <= 33; ++n)
    { // 30 is a keyframe: 28 and 29 are where it used to go wrong
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        INFO ("frame " << n << ": got " << frameIndexOf (img) << " at " << img.getActualTime());
        CHECK (frameIndexOf (img) == n);
        CHECK (img.getActualTime() == Time{ n, 30 });
        CHECK (img.isKeyframe() == (n % 30 == 0));
    }
}

// Fragmented MP4. The demuxer does not apply its reorder shift to fragmented files, so a
// seek to one of the last frames of a GOP lands on the next fragment's keyframe; the landing is
// caught at the packet level (no decode) and the pipeline learns the shift, so the second such
// request is exact.
TEST_CASE ("realtime: fragmented MP4 requests stay exact across fragments", "[realtime][sync][fmp4]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_frag.mp4").string(), swOptions()));
    auto far = REQUIRE_OK (g.imageAt (Time{ 89, 30 })); // park far away so the next request must seek
    CHECK (frameIndexOf (far) == 89);
    // Frame 29 is one of the last two frames of its GOP: a plain seek lands on the next fragment.
    auto a = REQUIRE_OK (g.imageAt (Time{ 29, 30 }));
    INFO ("frame 29: got " << frameIndexOf (a) << " at " << a.getActualTime());
    CHECK (frameIndexOf (a) == 29); // the overshot landing is corrected, not answered from
    CHECK (a.getActualTime() == Time{ 29, 30 });
    CHECK_FALSE (a.wasClamped());
    auto b = REQUIRE_OK (g.imageAt (Time{ 119, 30 }));
    CHECK (frameIndexOf (b) == 119);
    // The reorder shift is learned from that landing: the next such seek lands right the first time.
    auto c = REQUIRE_OK (g.imageAt (Time{ 59, 30 }));
    INFO ("frame 59: got " << frameIndexOf (c) << " at " << c.getActualTime());
    CHECK (frameIndexOf (c) == 59);
    CHECK (c.getActualTime() == Time{ 59, 30 });

    // Both ends of every fragment, forwards and backwards, in a random order.
    for (int n : { 0, 30, 31, 89, 60, 5, 118, 90, 119, 29, 61, 1 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        INFO ("frame " << n << ": got " << frameIndexOf (img) << " at " << img.getActualTime());
        CHECK (frameIndexOf (img) == n);
        CHECK (img.getActualTime() == Time{ n, 30 });
        CHECK_FALSE (img.wasClamped());
    }
}

// Open-GOP HEVC. The leading pictures of a CRA follow it in decode order, so a seek to
// them lands on the CRA and the decoder drops them. The overshoot is detected from the first key
// packet (no decode) and corrected with one cheap seek to the previous keyframe; a GOP is never
// decoded twice.
TEST_CASE ("realtime: open-GOP HEVC leading pictures are returned, not their CRA", "[realtime][sync][hevc]")
{
#if ! STILLS_HAVE_X265
    SKIP ("ffmpeg was built without libx265; fixture not generated");
#else
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_opengop_hevc.mp4").string(), swOptions()));
    CHECK (g.getInfo().codecName == "hevc");

    for (int n : { 28, 29, 58, 30, 59, 2, 31, 57, 0, 89 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        INFO ("frame " << n << ": got " << frameIndexOf (img) << " at " << img.getActualTime());
        CHECK (frameIndexOf (img) == n); // a leading picture is never answered with its CRA
        CHECK (img.getActualTime() == Time{ n, 30 });
        CHECK_FALSE (img.wasClamped());
    }
#endif
}

// Keyframe mode: while the held keyframe is the answer it must keep being the answer, exactly,
// and the step to the next keyframe must land on that keyframe and not inside its GOP.
TEST_CASE ("realtime: keyframe mode reuses the held keyframe", "[realtime][sync][keyframe]")
{
    stills::Options o = swOptions();
    o.tolerance = stills::Tolerance::any();
    auto g = openCounter (o);
    auto first = REQUIRE_OK (g.imageAt (Time{ 31, 30 }));
    CHECK (frameIndexOf (first) == 30);

    for (int n : { 32, 33, 45, 59, 30 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        INFO ("frame " << n << ": got " << frameIndexOf (img) << " at " << img.getActualTime());
        CHECK (frameIndexOf (img) == 30);
        CHECK (img.isKeyframe());
        CHECK (img.getActualTime() == Time{ 30, 30 });
        CHECK_FALSE (img.wasClamped());
    }

    auto next = REQUIRE_OK (g.imageAt (Time{ 61, 30 }));
    CHECK (frameIndexOf (next) == 60);
    CHECK (next.isKeyframe());
    CHECK (next.getActualTime() == Time{ 60, 30 });
    auto back = REQUIRE_OK (g.imageAt (Time{ 1, 30 }));
    CHECK (frameIndexOf (back) == 0);
    CHECK (back.isKeyframe());
    CHECK (back.getActualTime() == Time::zero());
}

// AVDISCARD_NONREF far from the target must never skip a frame whose display interval can reach the
// window. counter_vfr_hold.mp4 keeps 16 of every 100 source frames: frame 15 (a non-reference B) is
// on screen from 0.5 s to 3.33 s while every packet's stored duration is 1/30 s (mov stores
// decode-order deltas), so the skip decision must rest on an already-fed later frame, not on
// durations.
TEST_CASE ("realtime: NONREF never skips a held non-reference frame on VFR content", "[realtime][vfr][nonref]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_vfr_hold.mp4").string(), swOptions()));
    const std::pair<int, int> cases[] = { { 16, 15 },   { 20, 15 },   { 30, 15 },   { 60, 15 },   { 99, 15 },
                                          { 100, 100 }, { 112, 112 }, { 115, 115 }, { 150, 115 }, { 199, 115 },
                                          { 215, 215 }, { 250, 215 }, { 14, 14 },   { 15, 15 } };

    for (const auto& [n, expected] : cases)
    {
        (void)REQUIRE_OK (
            g.imageAt (Time{ 515, 30 })); // park far ahead: the next request decodes from keyframe 0 with NONREF
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        INFO ("t = " << n << "/30: got " << frameIndexOf (img) << " at " << img.getActualTime());
        CHECK (frameIndexOf (img) == expected);
        CHECK (img.getActualTime() == Time{ expected, 30 });
    }
}

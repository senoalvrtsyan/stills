// What the rest of the suite assumes about the generated clips. Every other test reads a fixture
// and checks the library against it; if a fixture is not what it is thought to be — a different
// encoder, a different ffmpeg, a silently changed flag — those tests fail somewhere far away, or
// worse, keep passing while testing something else. These assertions are the fixtures' contract.
#include <string>
#include <vector>

#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::Time;

namespace
{

struct Invariant
{
    const char* name;
    int frames;                 // presented frames, 0 = not checked
    std::vector<int> keyframes; // positions in the presented sequence that are keyframes, empty = not checked
    bool available = true;
};

} // namespace

TEST_CASE ("fixtures: the generated clips are what the rest of the suite assumes", "[fixtures]")
{
    const std::vector<Invariant> all = {
        // 4 s at 30 fps, keyframes forced at 0/30/60/90 (generate.sh, -force_key_frames).
        { "counter.mp4", 120, { 0, 30, 60, 90 } },
        { "counter_offset.ts", 120, { 0, 30, 60, 90 } },
        { "counter.mkv", 120, { 0, 30, 60, 90 } },
        { "counter_frag.mp4", 120, { 0, 30, 60, 90 } },
        { "counter_opengop.mp4", 120, {} },
        { "counter_ilace.mp4", 120, {} },
        // One IDR only: every seek away from the start lands with no keyframe ahead.
        { "counter_longgop.ts", 120, { 0 } },
        // Every third source frame kept: 40 presented frames, and test_playback relies on the 1st
        // and 28th of them being keyframes (source indices 0 and 81).
        { "counter_vfr.mp4", 40, { 0, 27 } },
        // -ss 1.1: the first presented frame is source index 33 and is not a keyframe.
        { "counter_editlist.mp4", 87, {} },
        { "counter_opengop_hevc.mp4", 120, {}, STILLS_HAVE_X265 != 0 },
        { "counter_vp9.webm", 120, {}, STILLS_HAVE_VP9 != 0 },
    };

    for (const Invariant& f : all)
    {
        if (! f.available) continue;
        DYNAMIC_SECTION (f.name)
        {
            auto g = REQUIRE_OK (AssetImageGenerator::open (fixture (f.name).string(), swOptions()));
            // Presented frames, counted the way the library presents them: one request per 1/30 s tick,
            // deduplicated by presentation time.
            std::vector<Time> distinct;
            std::vector<int> keyframePositions;

            for (int n = 0; n < 200; ++n)
            {
                auto img = g.imageAt (Time{ n, 30 });

                if (! img)
                {
                    CHECK (img.error().code == stills::ErrorCode::timeOutOfRange);
                    break;
                }

                if (distinct.empty() || distinct.back() != img->getActualTime())
                {
                    if (img->isKeyframe()) keyframePositions.push_back (static_cast<int> (distinct.size()));
                    distinct.push_back (img->getActualTime());
                }
            }

            INFO ("presented frames: " << distinct.size() << ", keyframes: " << keyframePositions.size());

            if (f.frames != 0) CHECK (static_cast<int> (distinct.size()) == f.frames);
            if (! f.keyframes.empty()) CHECK (keyframePositions == f.keyframes);
            // The luma index is what every other test recovers identity from: it must be readable and
            // strictly increasing across presented frames.
            auto first = REQUIRE_OK (g.imageAt (Time::zero()));
            const int firstIndex = frameIndexOf (first);
            CHECK (firstIndex >= 0);
            auto later = REQUIRE_OK (g.imageAt (distinct.at (distinct.size() / 2)));
            CHECK (frameIndexOf (later) > firstIndex);
        }
    }
}

// The reorder depth the fixtures were encoded with (bframes=2, b-adapt=0): a frame's presentation
// time runs ahead of its decode order, which is what the landing verification is written against.
TEST_CASE ("fixtures: counter.mp4 carries the B-frame reordering the tests assume", "[fixtures]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mp4").string(), swOptions()));
    const auto& info = g.getInfo();
    CHECK (info.codecName == "h264");
    CHECK (info.codedSize == stills::Size{ 64, 48 });
    CHECK (info.averageFrameRate == stills::Rational{ 30, 1 });
    REQUIRE (info.duration.has_value());
    CHECK (*info.duration == Time{ 4, 1 });
    CHECK (info.timeBase == stills::Rational{ 1, 15360 }); // frame k is stored at pts k * 512
    // A frame in the middle of a reordered GOP: exact, and reported at exactly its own time.
    auto img = REQUIRE_OK (g.imageAt (Time{ 47, 30 }));
    CHECK (frameIndexOf (img) == 47);
    CHECK (img.getActualTime() == Time{ 47, 30 });
}

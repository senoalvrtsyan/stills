// What the pipeline may conclude from state left behind by an earlier request: the last frame's
// display interval, frames skipped by an abandoned request, and a held frame that is not the
// presentation-order tail. Each of these can return a wrong frame, or no frame at all, with no
// error and no way for a consumer to notice.
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "support/stills_TestCommon.h"

using namespace testsupport;
using namespace std::chrono_literals;
using stills::AssetImageGenerator;
using stills::Completion;
using stills::ErrorCode;
using stills::OutOfRangePolicy;
using stills::Time;

namespace
{

stills::Options boundsOptions (OutOfRangePolicy policy)
{
    stills::Options o = swOptions();
    o.outOfRange = policy;
    return o;
}

} // namespace

// A time past the last frame but inside the one-frame slack the bounds pre-check allows must
// answer the same way every time, whatever an earlier request left behind: a held frame that
// claims to cover every time at or after its pts would come back as an unflagged Image.
TEST_CASE ("positioning: a past-the-end request answers the same however the generator got there", "[sync][bounds]")
{
    struct Case
    {
        const char* fixture;
        Time pastTheEnd; // beyond the last frame, inside the pre-check's one-frame slack
        Time elsewhere;  // a far-away time that moves held and starts a new run
    };

    const Case cases[] = {
        { "counter.mp4", Time{ 402, 100 }, Time{ 1, 3 } },    // ends at 4.0 s
        { "counter_vfr.mp4", Time{ 119, 30 }, Time{ 1, 3 } }, // ends at 3.9333 s
    };

    for (const Case& c : cases)
    {
        DYNAMIC_SECTION (c.fixture)
        {
            {
                INFO ("OutOfRangePolicy::error");
                auto g = REQUIRE_OK (
                    AssetImageGenerator::open (fixture (c.fixture).string(), boundsOptions (OutOfRangePolicy::error)));
                REQUIRE_ERROR (g.imageAt (c.pastTheEnd), ErrorCode::timeOutOfRange);
                (void)REQUIRE_OK (g.imageAt (c.elsewhere)); // moves held, starts a new run
                // The same request on the same generator must not answer differently because of history.
                REQUIRE_ERROR (g.imageAt (c.pastTheEnd), ErrorCode::timeOutOfRange);
            }

            {
                INFO ("OutOfRangePolicy::clampToLastFrame");
                auto g = REQUIRE_OK (AssetImageGenerator::open (fixture (c.fixture).string(),
                                                                boundsOptions (OutOfRangePolicy::clampToLastFrame)));
                auto first = REQUIRE_OK (g.imageAt (c.pastTheEnd));
                CHECK (first.wasClamped());
                const int lastIndex = frameIndexOf (first);
                (void)REQUIRE_OK (g.imageAt (c.elsewhere));
                auto again = REQUIRE_OK (g.imageAt (c.pastTheEnd));
                CHECK (frameIndexOf (again) == lastIndex);
                CHECK (again.wasClamped()); // the clamp must survive the second answer
            }
        }
    }
}

// A request cancelled before it reached its target fed packets to the decoder under *its* skip
// window, so frames between the decoder's frontier and those packets may never be produced. The
// next forward request must not conclude that the frame it is holding covers the requested time.
//
// counter.mp4: GOP 30, IDR at 60, bframes=2 b-adapt=0. imageAt(61/30) positions inside the GOP;
// a batch request for a later frame of the same GOP feeds B65 with AVDISCARD_NONREF; cancelling it
// leaves frame 65 skipped; imageAt(65/30) then returned frame 64 with frame 64's getActualTime().
TEST_CASE ("positioning: a cancelled request does not make the next one return the previous frame",
           "[async][cancel][sweep]")
{
    const stills::Options o = swOptions();
    int checked = 0;

    for (const int target : { 70, 85, 88 })
    {
        for (int cut = 1; cut <= 4; ++cut)
        {
            auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mp4").string(), o));
            auto anchor = REQUIRE_OK (g.imageAt (Time{ 61, 30 }));
            REQUIRE (frameIndexOf (anchor) == 61);
            std::atomic<bool> done{ false };
            auto req = g.generateImages ({ Time{ target, 30 } }, [&] (Completion) { done.store (true); });

            // Cancel part-way to the target, at four depths into the forward decode. Decoding the
            // ~27 frames between the anchor and the target takes on the order of a millisecond here,
            // so 100 us steps land inside it; `done` keeps the wait bounded if it ever does not.
            for (int i = 0; i < cut && ! done.load(); ++i)
                std::this_thread::sleep_for (std::chrono::microseconds{ 100 });
            req.cancel();
            REQUIRE (req.waitFor (30s) == stills::WaitResult::finished);

            for (int n = 62; n < 70; ++n)
            {
                auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
                INFO ("target " << target << " cut " << cut << " tick " << n << ": got " << frameIndexOf (img) << " at "
                                << img.getActualTime());
                CHECK (frameIndexOf (img) == n);
                CHECK (img.getActualTime() == Time{ n, 30 });
                ++checked;
            }
        }
    }

    CHECK (checked == 3 * 4 * 8);
}

// M1: an MPEG-TS whose timestamps jump backwards (two recordings concatenated). Decoding forward
// across the jump to the end leaves the last decoded frame *below* every later request, and the
// end-of-stream shortcut concluded from it without ever repositioning: every later request,
// including Time::zero(), failed timeOutOfRange with no seek attempted.
TEST_CASE ("positioning: the generator stays usable after scanning across a backwards discontinuity",
           "[containers][bounds]")
{
    auto g =
        REQUIRE_OK (AssetImageGenerator::open (fixture ("disc.ts").string(), boundsOptions (OutOfRangePolicy::error)));

    // Scan sequentially into the second segment, where the timestamps go backwards.
    for (int n = 0; n < 120; ++n)
    {
        auto img = g.imageAt (Time{ n, 30 });

        if (! img)
        {
            INFO ("tick " << n << ": " << img.error());
            CHECK (img.error().code == ErrorCode::timeOutOfRange);
            break;
        }
    }

    // Whatever the scan concluded, the generator must still answer requests it answered before.
    for (const int n : { 0, 30, 50, 60 })
    {
        auto img = g.imageAt (Time{ n, 30 });
        INFO ("after the scan, tick " << n << ": " << (img ? std::string{ "ok" } : toString (img.error())));
        REQUIRE (img.has_value());
        CHECK (frameIndexOf (*img) == n);
    }
}

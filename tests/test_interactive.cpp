// The interactive path: per-request options.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

#include "support/stills_TestCommon.h"

using namespace testsupport;
using namespace std::chrono_literals;
using stills::AssetImageGenerator;
using stills::Completion;
using stills::GenerationStatus;
using stills::RequestOptions;
using stills::Time;

// Tolerance and output size per request, on one generator.
TEST_CASE ("interactive: RequestOptions override tolerance and size per call", "[realtime][options]")
{
    auto g = openCounter (swOptions (stills::PixelFormat::rgba));
    RequestOptions any;
    any.tolerance = stills::Tolerance::any();
    auto k = REQUIRE_OK (g.imageAt (Time{ 47, 30 }, any));
    CHECK (frameIndexOf (k) == 30);
    CHECK (k.isKeyframe());
    auto exact = REQUIRE_OK (g.imageAt (Time{ 47, 30 }));
    CHECK (frameIndexOf (exact) == 47);
    RequestOptions small;
    small.maximumSize = stills::Size{ 32, 0 };
    auto thumb = REQUIRE_OK (g.imageAt (Time{ 47, 30 }, small));
    CHECK (thumb.getSize() == stills::Size{ 32, 24 });
    CHECK (frameIndexOf (thumb) == 47);
    auto native = REQUIRE_OK (g.imageAt (Time{ 48, 30 }));
    CHECK (native.getSize() == stills::Size{ 64, 48 });
    RequestOptions bad;
    bad.tolerance = stills::Tolerance{ Time{ -1, 30 }, Time::zero() };
    REQUIRE_ERROR (g.imageAt (Time{ 1, 30 }, bad), stills::ErrorCode::invalidArgument);
    RequestOptions tiny;
    tiny.maximumSize = stills::Size{ 1, 1 };
    auto g420 = openCounter (swOptions (stills::PixelFormat::yuv420p));
    REQUIRE_ERROR (g420.imageAt (Time{ 1, 30 }, tiny), stills::ErrorCode::invalidArgument);
    // Async: options travel with the batch; a bad override fails per item, not the whole batch.
    Collector col;
    auto req = g.generateImages ({ Time{ 10, 30 }, Time{ 20, 30 } }, col.handler(), any);
    REQUIRE (req.waitFor (30s) == stills::WaitResult::finished);
    REQUIRE (col.count (GenerationStatus::succeeded) == 2);

    for (const auto& item : col.getItems())
        CHECK ((*item.index == 0 || *item.index == 0)); // keyframe 0 covers 10 and 20
    Collector col2;
    auto req2 = g.generateImages ({ Time{ 10, 30 } }, col2.handler(), bad);
    REQUIRE (req2.waitFor (30s) == stills::WaitResult::finished);
    CHECK (col2.count (GenerationStatus::failed) == 1);
}

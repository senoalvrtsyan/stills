#include <format>
#include <functional>
#include <optional>
#include <stills/stills_Interop.h>
#include <type_traits>
#include <vector>

#include "support/common.hpp"

using namespace testsupport;
using stills::Image;
using stills::Time;

static_assert (! std::is_copy_constructible_v<Image>);
static_assert (! std::is_copy_assignable_v<Image>);
static_assert (std::is_nothrow_move_constructible_v<Image>);
static_assert (! std::is_default_constructible_v<Image>);
static_assert (! std::is_copy_constructible_v<stills::AssetImageGenerator>);
static_assert (std::is_nothrow_move_constructible_v<stills::AssetImageGenerator>);

TEST_CASE ("image: move semantics and emptiness", "[image]")
{
    auto g = openCounter (swOptions (stills::PixelFormat::rgba));
    Image a = REQUIRE_OK (g.imageAt (Time{ 10, 30 }));
    REQUIRE_FALSE (a.isEmpty());
    Image b = std::move (a);
    CHECK (a.isEmpty()); // NOLINT(bugprone-use-after-move) — testing the moved-from contract
    CHECK (a.getPixels().empty());
    CHECK (a.getPlaneCount() == 0);
    CHECK (a.getSize() == stills::Size{});
    CHECK (a.getPackedSizeBytes() == 0);
    REQUIRE_ERROR (a.copyPackedTo ({}), stills::ErrorCode::invalidState);
    REQUIRE_ERROR (a.clone(), stills::ErrorCode::invalidState);
    REQUIRE_ERROR (a.toPackedBytes(), stills::ErrorCode::invalidState);
    CHECK_FALSE (b.isEmpty());
    CHECK (frameIndexOf (b) == 10);
    CHECK (b.getActualTime() == Time{ 10, 30 });
}

TEST_CASE ("image: packed copies and clone", "[image]")
{
    auto g = openCounter (swOptions (stills::PixelFormat::rgb24));
    Image img = REQUIRE_OK (g.imageAt (Time{ 77, 30 }));
    const std::size_t need = img.getPackedSizeBytes();
    CHECK (need == 64u * 48u * 3u);
    std::vector<std::byte> small (need - 1);
    REQUIRE_ERROR (img.copyPackedTo (small), stills::ErrorCode::invalidArgument);
    std::vector<std::byte> buf (need);
    REQUIRE_OK (img.copyPackedTo (buf));
    CHECK (buf == REQUIRE_OK (img.toPackedBytes()));
    Image copy = REQUIRE_OK (img.clone());
    CHECK (copy.getSize() == img.getSize());
    CHECK (REQUIRE_OK (copy.toPackedBytes()) == buf);
    CHECK (copy.getActualTime() == img.getActualTime());
    CHECK (frameIndexOf (copy) == 77);
}

// The README promises an Image outlives the generator that produced it — the output buffers are
// reference-counted out of a pool, not owned by the pipeline. Every other test reads its Image
// while the generator is still alive, so nothing actually held that promise to account.
TEST_CASE ("image: an image stays readable after its generator is destroyed", "[image][lifetime]")
{
    std::optional<Image> img;
    std::vector<std::byte> expected;
    Time actual{};
    {
        auto g = openCounter (swOptions (stills::PixelFormat::rgba));
        img.emplace (REQUIRE_OK (g.imageAt (Time{ 42, 30 })));
        expected = REQUIRE_OK (img->toPackedBytes());
        actual = img->getActualTime();
        g.close(); // joins the worker and frees every libav object the pipeline held
    }

    REQUIRE_FALSE (img->isEmpty());
    CHECK (img->getActualTime() == actual);
    CHECK (frameIndexOf (*img) == 42);                     // reads the pixels through getPlane()
    CHECK (REQUIRE_OK (img->toPackedBytes()) == expected); // byte-identical, not merely readable
    Image cloned = REQUIRE_OK (img->clone());              // and still clonable
    img.reset();                                           // releases the pooled buffer
    CHECK (frameIndexOf (cloned) == 42);
    CHECK (REQUIRE_OK (cloned.toPackedBytes()) == expected);
}

TEST_CASE ("image: interop escape hatch", "[image][interop]")
{
    auto g = openCounter (swOptions (stills::PixelFormat::rgba));
    Image img = REQUIRE_OK (g.imageAt (Time{ 5, 30 }));
    const AVFrame* borrowed = stills::interop::getNativeFrame (img);
    REQUIRE (borrowed != nullptr);
    CHECK (borrowed->width == 64);
    CHECK (borrowed->format == AV_PIX_FMT_RGBA);

    AVFrame* owned = stills::interop::release (std::move (img));
    REQUIRE (owned != nullptr);
    CHECK (img.isEmpty()); // NOLINT(bugprone-use-after-move)
    CHECK (owned->height == 48);

    // Adopt it back, then let the Image free it.
    auto adopted = REQUIRE_OK (stills::interop::adoptFrame (owned, stills::Rational{ 1, 15360 }));
    CHECK (adopted.getSize() == stills::Size{ 64, 48 });
    CHECK (frameIndexOf (adopted) == 5);
    CHECK (adopted.getActualTime() == Time{ 5, 30 });

    REQUIRE_ERROR (stills::interop::adoptFrame (nullptr), stills::ErrorCode::invalidArgument);
    AVFrame* bare = av_frame_alloc();
    bare->format = AV_PIX_FMT_YUV420P10LE; // unsupported output format
    bare->width = 4;
    bare->height = 4;
    REQUIRE (av_frame_get_buffer (bare, 0) == 0);
    REQUIRE_ERROR (stills::interop::adoptFrame (bare), stills::ErrorCode::invalidArgument);
    av_frame_free (&bare); // still ours after a failed adopt

    AVFrame* flipped = av_frame_alloc();
    flipped->format = AV_PIX_FMT_RGBA;
    flipped->width = 4;
    flipped->height = 4;
    REQUIRE (av_frame_get_buffer (flipped, 0) == 0);
    flipped->data[0] += static_cast<std::ptrdiff_t> (flipped->linesize[0]) * 3; // bottom-up view
    flipped->linesize[0] = -flipped->linesize[0];
    REQUIRE_ERROR (stills::interop::adoptFrame (flipped), stills::ErrorCode::invalidArgument);
    flipped->linesize[0] = -flipped->linesize[0];
    flipped->data[0] -= static_cast<std::ptrdiff_t> (flipped->linesize[0]) * 3;
    av_frame_free (&flipped);

    CHECK (stills::interop::toAv (stills::PixelFormat::nv12) == AV_PIX_FMT_NV12);
    CHECK (stills::interop::fromAv (AV_PIX_FMT_BGRA) == stills::PixelFormat::bgra);
    CHECK_FALSE (stills::interop::fromAv (AV_PIX_FMT_YUV444P).has_value());
}

// wasClamped() answered true for three unrelated conditions, one of which is not a defect at all:
// on an edit-list file in nearest-keyframe mode the first presented frame *is* the right answer.
// getAdjustment() says which, and wasClamped() stays as the one-bit question.
TEST_CASE ("image: getAdjustment() distinguishes the reasons wasClamped() is true", "[image][bounds]")
{
    SECTION ("the frame asked for")
    {
        auto g = openCounter();
        auto img = REQUIRE_OK (g.imageAt (Time{ 45, 30 }));
        CHECK (img.getAdjustment() == stills::Adjustment::none);
        CHECK_FALSE (img.wasClamped());
    }

    SECTION ("past the last frame")
    {
        stills::Options o = swOptions();
        o.outOfRange = stills::OutOfRangePolicy::clampToLastFrame;
        auto g = openCounter (o);
        auto img = REQUIRE_OK (g.imageAt (Time{ 600, 30 }));
        CHECK (img.getAdjustment() == stills::Adjustment::clampedToLast);
        CHECK (img.wasClamped());
        CHECK (toString (img.getAdjustment()) == "clampedToLast");
    }

    SECTION ("every keyframe before the request was trimmed away by an edit list")
    {
        stills::Options o = swOptions();
        o.tolerance = stills::Tolerance::any();
        auto g = REQUIRE_OK (stills::AssetImageGenerator::open (fixture ("counter_editlist.mp4").string(), o));
        auto img = REQUIRE_OK (g.imageAt (Time::zero()));
        CHECK (img.getAdjustment() == stills::Adjustment::keyframeBeforeEdit);
        CHECK (img.wasClamped()); // unchanged for consumers reading the bool
    }
}

// Every enum the library hands back formats and prints, so a log line does not depend on which
// ones happened to have a formatter.
TEST_CASE ("image: every public enum formats and prints", "[image][api]")
{
    CHECK (std::format ("{}", stills::PixelFormat::yuv420p) == "yuv420p");
    CHECK (std::format ("{}", stills::ColorRange::limited) == "limited");
    CHECK (std::format ("{}", stills::HardwareDeviceType::vaapi) == "vaapi");
    CHECK (std::format ("{}", stills::HardwarePolicy::softwareOnly) == "softwareOnly");
    CHECK (std::format ("{}", stills::Scaler::bicubic) == "bicubic");
    CHECK (std::format ("{}", stills::OutOfRangePolicy::error) == "error");
    CHECK (std::format ("{}", stills::GenerationStatus::succeeded) == "succeeded");
    CHECK (std::format ("{}", stills::WaitResult::finished) == "finished");
    CHECK (std::format ("{}", stills::Adjustment::clampedToLast) == "clampedToLast");
    CHECK (std::format ("{:>10}", stills::Scaler::bicubic) == "   bicubic"); // width and fill work
    CHECK (std::format ("{}", stills::Size{ 64, 48 }) == "64x48");
    CHECK (std::format ("{}", stills::ErrorCode::invalidState) == "invalidState");
    // Size is ordered and hashable, so it can key a container.
    CHECK (stills::Size{ 64, 48 } < stills::Size{ 64, 49 });
    CHECK (std::hash<stills::Size>{}(stills::Size{ 64, 48 }) == std::hash<stills::Size>{}(stills::Size{ 64, 48 }));
}

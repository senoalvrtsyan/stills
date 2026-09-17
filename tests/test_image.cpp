#include <format>
#include <functional>
#include <optional>
#include <stills/interop.hpp>
#include <type_traits>
#include <vector>

#include "support/common.hpp"

using namespace testsupport;
using stills::Image;
using stills::Time;

static_assert(!std::is_copy_constructible_v<Image>);
static_assert(!std::is_copy_assignable_v<Image>);
static_assert(std::is_nothrow_move_constructible_v<Image>);
static_assert(!std::is_default_constructible_v<Image>);
static_assert(!std::is_copy_constructible_v<stills::AssetImageGenerator>);
static_assert(std::is_nothrow_move_constructible_v<stills::AssetImageGenerator>);

TEST_CASE("image: move semantics and emptiness", "[image]") {
  auto g = open_counter(sw_options(stills::PixelFormat::rgba));
  Image a = REQUIRE_OK(g.image_at(Time{10, 30}));
  REQUIRE_FALSE(a.empty());
  Image b = std::move(a);
  CHECK(a.empty());  // NOLINT(bugprone-use-after-move) — testing the moved-from contract
  CHECK(a.pixels().empty());
  CHECK(a.plane_count() == 0);
  CHECK(a.size() == stills::Size{});
  CHECK(a.packed_size_bytes() == 0);
  REQUIRE_ERROR(a.copy_packed_to({}), stills::ErrorCode::invalid_state);
  REQUIRE_ERROR(a.clone(), stills::ErrorCode::invalid_state);
  REQUIRE_ERROR(a.to_packed_bytes(), stills::ErrorCode::invalid_state);
  CHECK_FALSE(b.empty());
  CHECK(frame_index_of(b) == 10);
  CHECK(b.actual_time() == Time{10, 30});
}

TEST_CASE("image: packed copies and clone", "[image]") {
  auto g = open_counter(sw_options(stills::PixelFormat::rgb24));
  Image img = REQUIRE_OK(g.image_at(Time{77, 30}));
  const std::size_t need = img.packed_size_bytes();
  CHECK(need == 64u * 48u * 3u);
  std::vector<std::byte> small(need - 1);
  REQUIRE_ERROR(img.copy_packed_to(small), stills::ErrorCode::invalid_argument);
  std::vector<std::byte> buf(need);
  REQUIRE_OK(img.copy_packed_to(buf));
  CHECK(buf == REQUIRE_OK(img.to_packed_bytes()));
  Image copy = REQUIRE_OK(img.clone());
  CHECK(copy.size() == img.size());
  CHECK(REQUIRE_OK(copy.to_packed_bytes()) == buf);
  CHECK(copy.actual_time() == img.actual_time());
  CHECK(frame_index_of(copy) == 77);
}

// The README promises an Image outlives the generator that produced it — the output buffers are
// reference-counted out of a pool, not owned by the pipeline. Every other test reads its Image
// while the generator is still alive, so nothing actually held that promise to account.
TEST_CASE("image: an image stays readable after its generator is destroyed", "[image][lifetime]") {
  std::optional<Image> img;
  std::vector<std::byte> expected;
  Time actual{};
  {
    auto g = open_counter(sw_options(stills::PixelFormat::rgba));
    img.emplace(REQUIRE_OK(g.image_at(Time{42, 30})));
    expected = REQUIRE_OK(img->to_packed_bytes());
    actual = img->actual_time();
    g.close();  // joins the worker and frees every libav object the pipeline held
  }
  REQUIRE_FALSE(img->empty());
  CHECK(img->actual_time() == actual);
  CHECK(frame_index_of(*img) == 42);                      // reads the pixels through plane()
  CHECK(REQUIRE_OK(img->to_packed_bytes()) == expected);  // byte-identical, not merely readable
  Image cloned = REQUIRE_OK(img->clone());                // and still clonable
  img.reset();                                            // releases the pooled buffer
  CHECK(frame_index_of(cloned) == 42);
  CHECK(REQUIRE_OK(cloned.to_packed_bytes()) == expected);
}

TEST_CASE("image: interop escape hatch", "[image][interop]") {
  auto g = open_counter(sw_options(stills::PixelFormat::rgba));
  Image img = REQUIRE_OK(g.image_at(Time{5, 30}));
  const AVFrame* borrowed = stills::interop::native_frame(img);
  REQUIRE(borrowed != nullptr);
  CHECK(borrowed->width == 64);
  CHECK(borrowed->format == AV_PIX_FMT_RGBA);

  AVFrame* owned = stills::interop::release(std::move(img));
  REQUIRE(owned != nullptr);
  CHECK(img.empty());  // NOLINT(bugprone-use-after-move)
  CHECK(owned->height == 48);

  // Adopt it back, then let the Image free it.
  auto adopted = REQUIRE_OK(stills::interop::adopt_frame(owned, stills::Rational{1, 15360}));
  CHECK(adopted.size() == stills::Size{64, 48});
  CHECK(frame_index_of(adopted) == 5);
  CHECK(adopted.actual_time() == Time{5, 30});

  REQUIRE_ERROR(stills::interop::adopt_frame(nullptr), stills::ErrorCode::invalid_argument);
  AVFrame* bare = av_frame_alloc();
  bare->format = AV_PIX_FMT_YUV420P10LE;  // unsupported output format
  bare->width = 4;
  bare->height = 4;
  REQUIRE(av_frame_get_buffer(bare, 0) == 0);
  REQUIRE_ERROR(stills::interop::adopt_frame(bare), stills::ErrorCode::invalid_argument);
  av_frame_free(&bare);  // still ours after a failed adopt

  AVFrame* flipped = av_frame_alloc();
  flipped->format = AV_PIX_FMT_RGBA;
  flipped->width = 4;
  flipped->height = 4;
  REQUIRE(av_frame_get_buffer(flipped, 0) == 0);
  flipped->data[0] += static_cast<std::ptrdiff_t>(flipped->linesize[0]) * 3;  // bottom-up view
  flipped->linesize[0] = -flipped->linesize[0];
  REQUIRE_ERROR(stills::interop::adopt_frame(flipped), stills::ErrorCode::invalid_argument);
  flipped->linesize[0] = -flipped->linesize[0];
  flipped->data[0] -= static_cast<std::ptrdiff_t>(flipped->linesize[0]) * 3;
  av_frame_free(&flipped);

  CHECK(stills::interop::to_av(stills::PixelFormat::nv12) == AV_PIX_FMT_NV12);
  CHECK(stills::interop::from_av(AV_PIX_FMT_BGRA) == stills::PixelFormat::bgra);
  CHECK_FALSE(stills::interop::from_av(AV_PIX_FMT_YUV444P).has_value());
}

// was_clamped() answered true for three unrelated conditions, one of which is not a defect at all:
// on an edit-list file in nearest-keyframe mode the first presented frame *is* the right answer.
// adjustment() says which, and was_clamped() stays as the one-bit question.
TEST_CASE("image: adjustment() distinguishes the reasons was_clamped() is true",
          "[image][bounds]") {
  SECTION("the frame asked for") {
    auto g = open_counter();
    auto img = REQUIRE_OK(g.image_at(Time{45, 30}));
    CHECK(img.adjustment() == stills::Adjustment::none);
    CHECK_FALSE(img.was_clamped());
  }
  SECTION("past the last frame") {
    stills::Options o = sw_options();
    o.out_of_range = stills::OutOfRangePolicy::clamp_to_last_frame;
    auto g = open_counter(o);
    auto img = REQUIRE_OK(g.image_at(Time{600, 30}));
    CHECK(img.adjustment() == stills::Adjustment::clamped_to_last);
    CHECK(img.was_clamped());
    CHECK(to_string(img.adjustment()) == "clamped_to_last");
  }
  SECTION("every keyframe before the request was trimmed away by an edit list") {
    stills::Options o = sw_options();
    o.tolerance = stills::Tolerance::any();
    auto g =
        REQUIRE_OK(stills::AssetImageGenerator::open(fixture("counter_editlist.mp4").string(), o));
    auto img = REQUIRE_OK(g.image_at(Time::zero()));
    CHECK(img.adjustment() == stills::Adjustment::keyframe_before_edit);
    CHECK(img.was_clamped());  // unchanged for consumers reading the bool
  }
}

// Every enum the library hands back formats and prints, so a log line does not depend on which
// ones happened to have a formatter.
TEST_CASE("image: every public enum formats and prints", "[image][api]") {
  CHECK(std::format("{}", stills::PixelFormat::yuv420p) == "yuv420p");
  CHECK(std::format("{}", stills::ColorRange::limited) == "limited");
  CHECK(std::format("{}", stills::HardwareDeviceType::vaapi) == "vaapi");
  CHECK(std::format("{}", stills::HardwarePolicy::software_only) == "software_only");
  CHECK(std::format("{}", stills::Scaler::bicubic) == "bicubic");
  CHECK(std::format("{}", stills::OutOfRangePolicy::error) == "error");
  CHECK(std::format("{}", stills::GenerationStatus::succeeded) == "succeeded");
  CHECK(std::format("{}", stills::WaitResult::finished) == "finished");
  CHECK(std::format("{}", stills::Adjustment::clamped_to_last) == "clamped_to_last");
  CHECK(std::format("{:>10}", stills::Scaler::bicubic) == "   bicubic");  // width and fill work
  CHECK(std::format("{}", stills::Size{64, 48}) == "64x48");
  CHECK(std::format("{}", stills::ErrorCode::invalid_state) == "invalid_state");
  // Size is ordered and hashable, so it can key a container.
  CHECK(stills::Size{64, 48} < stills::Size{64, 49});
  CHECK(std::hash<stills::Size>{}(stills::Size{64, 48}) ==
        std::hash<stills::Size>{}(stills::Size{64, 48}));
}

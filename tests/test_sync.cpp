#include <algorithm>
#include <random>

#include "support/common.hpp"

using namespace testsupport;
using namespace std::chrono_literals;
using stills::ErrorCode;
using stills::PixelFormat;
using stills::Time;

TEST_CASE("sync: frame-accurate extraction at exact frame times", "[sync][accuracy]") {
  const PixelFormat fmt = GENERATE(PixelFormat::yuv420p, PixelFormat::rgba);
  auto g = open_counter(sw_options(fmt));
  for (int n : {0, 1, 15, 29, 30, 31, 59, 60, 89, 90, 91, 118, 119}) {
    INFO("frame " << n << " format " << fmt);
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    CHECK(frame_index_of(img) == n);
    CHECK(img.actual_time() == Time{n, 30});
    CHECK(img.is_keyframe() == (n % 30 == 0));
    CHECK_FALSE(img.was_clamped());
    CHECK_FALSE(img.is_corrupt());
  }
}

TEST_CASE("sync: times between frames resolve to the frame being displayed", "[sync][accuracy]") {
  auto g = open_counter();
  SECTION("midpoints") {
    for (int n : {0, 14, 29, 30, 77, 119}) {
      auto img = REQUIRE_OK(g.image_at(Time{2 * n + 1, 60}));
      CHECK(frame_index_of(img) == n);
      CHECK(img.actual_time() == Time{n, 30});
    }
  }
  SECTION("requests round to the nearest stream tick") {
    // One tick of counter.mp4 is 1/15360 s (65 us). More than half a tick before a boundary is
    // the previous frame; within half a tick it is the frame at the boundary (see README).
    for (int n : {1, 30, 60, 119}) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30} - Time{1, 15360}));  // one full tick early
      CHECK(frame_index_of(img) == n - 1);
      img = REQUIRE_OK(g.image_at(Time{n, 30} - 1us));  // a quarter of a tick early
      CHECK(frame_index_of(img) == n);
      img = REQUIRE_OK(g.image_at(Time{n, 30} + 1us));
      CHECK(frame_index_of(img) == n);
    }
  }
  SECTION("chrono literal requests") {
    auto img = REQUIRE_OK(g.image_at(1500ms));
    CHECK(frame_index_of(img) == 45);
    img = REQUIRE_OK(g.image_at(Time::seconds(2.0)));
    CHECK(frame_index_of(img) == 60);
  }
}

TEST_CASE("sync: bounds", "[sync][bounds]") {
  SECTION("beyond the last frame is an error by default") {
    auto g = open_counter();
    REQUIRE_ERROR(g.image_at(Time{4, 1}), ErrorCode::time_out_of_range);
    REQUIRE_ERROR(g.image_at(Time{10, 1}), ErrorCode::time_out_of_range);
    auto img = REQUIRE_OK(g.image_at(Time{4, 1} - Time{1, 60}));
    CHECK(frame_index_of(img) == 119);
    CHECK_FALSE(img.was_clamped());
  }
  SECTION("clamp policy returns the last frame, flagged") {
    stills::Options o = sw_options();
    o.out_of_range = stills::OutOfRangePolicy::clamp_to_last_frame;
    auto g = open_counter(o);
    auto img = REQUIRE_OK(g.image_at(Time{10, 1}));
    CHECK(frame_index_of(img) == 119);
    CHECK(img.actual_time() == Time{119, 30});
    CHECK(img.was_clamped());
    img = REQUIRE_OK(g.image_at(Time{4, 1}));
    CHECK(frame_index_of(img) == 119);
    CHECK(img.was_clamped());
  }
  SECTION("negative and non-finite times are rejected") {
    auto g = open_counter();
    REQUIRE_ERROR(g.image_at(Time{-1, 30}), ErrorCode::invalid_argument);
    REQUIRE_ERROR(g.image_at(Time::invalid()), ErrorCode::invalid_argument);
    REQUIRE_ERROR(g.image_at(Time::positive_infinity()), ErrorCode::invalid_argument);
    auto img = REQUIRE_OK(g.image_at(Time::zero()));  // still usable afterwards
    CHECK(frame_index_of(img) == 0);
  }
}

TEST_CASE("sync: request order does not affect results", "[sync][accuracy]") {
  auto g = open_counter();
  SECTION("monotonic sweep uses the forward fast path") {
    for (int n = 0; n < 120; ++n) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      REQUIRE(frame_index_of(img) == n);
    }
  }
  SECTION("random permutation") {
    std::vector<int> order(120);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937{42});
    for (int n : order) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      REQUIRE(frame_index_of(img) == n);
    }
  }
  SECTION("GOP boundary forward and backward") {
    for (int n : {29, 30, 31, 31, 30, 29, 60, 59, 61}) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      REQUIRE(frame_index_of(img) == n);
    }
  }
  SECTION("repeated identical requests") {
    for (int i = 0; i < 3; ++i) {
      auto img = REQUIRE_OK(g.image_at(Time{47, 30}));
      REQUIRE(frame_index_of(img) == 47);
    }
  }
}

TEST_CASE("sync: variable frame rate uses the frame's own interval", "[sync][vfr]") {
  auto g = REQUIRE_OK(
      stills::AssetImageGenerator::open(fixture("counter_vfr.mp4").string(), sw_options()));
  // Every third source frame survives: 0, 3, 6, ...; a request inside a gap gets the frame on
  // screen.
  for (int n : {0, 1, 2, 3, 29, 30, 31, 100}) {
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    const int expected = (n / 3) * 3;
    CHECK(frame_index_of(img) == expected);
    CHECK(img.actual_time() == Time{expected, 30});
  }
}

#if __has_include(<unistd.h>)
#include <fcntl.h>
#include <unistd.h>
#define STILLS_HAVE_POSIX_FD 1
#endif

TEST_CASE("sync: raw elementary stream has no timestamps or index", "[sync][raw]") {
  auto g =
      REQUIRE_OK(stills::AssetImageGenerator::open(fixture("counter.h264").string(), sw_options()));
  CHECK(g.info().container_name == "h264");
  CHECK_FALSE(g.info().duration.has_value());
  // The container has no timestamps; the pipeline stamps frames in display order using the
  // codec-level frame rate (SPS timing), so requests are exact and time zero is the first frame.
  CHECK(g.info().average_frame_rate == stills::Rational{30, 1});
  for (int k : {0, 5, 40, 41, 100, 30, 3, 90, 119}) {  // 30 and 3 go backwards: forces a re-open
    auto img = REQUIRE_OK(g.image_at(Time{k, 30}));
    INFO("request " << k);
    CHECK(frame_index_of(img) == k);
    CHECK(img.actual_time() == Time{k, 30});
  }
  REQUIRE_ERROR(g.image_at(Time{120, 30}),
                ErrorCode::time_out_of_range);  // decided at EOF (no duration)
}

#if STILLS_HAVE_POSIX_FD
TEST_CASE("sync: non-seekable pipe input supports forward requests only", "[sync][pipe]") {
  const int fd = ::open(fixture("counter.mp4").c_str(), O_RDONLY);
  REQUIRE(fd >= 0);
  {
    auto g =
        REQUIRE_OK(stills::AssetImageGenerator::open("pipe:" + std::to_string(fd), sw_options()));
    for (int k : {0, 10, 50}) {
      auto img = REQUIRE_OK(g.image_at(Time{k, 30}));
      CHECK(frame_index_of(img) == k);
    }
    REQUIRE_ERROR(g.image_at(Time{20, 30}), ErrorCode::not_seekable);  // cannot rewind a pipe
    auto img = REQUIRE_OK(g.image_at(Time{60, 30}));                   // still usable going forward
    CHECK(frame_index_of(img) == 60);
    REQUIRE_ERROR(g.image_at(Time{7, 30}), ErrorCode::not_seekable);
  }
  ::close(fd);
  SECTION("Tolerance::any() falls back to exact selection without a usable seek") {
    const int fd2 = ::open(fixture("counter.mp4").c_str(), O_RDONLY);
    REQUIRE(fd2 >= 0);
    stills::Options o = sw_options();
    o.tolerance = stills::Tolerance::any();
    auto g = REQUIRE_OK(stills::AssetImageGenerator::open("pipe:" + std::to_string(fd2), o));
    auto img = REQUIRE_OK(g.image_at(Time{47, 30}));
    CHECK(frame_index_of(img) == 47);  // not an arbitrary later frame
    ::close(fd2);
  }
}
#endif

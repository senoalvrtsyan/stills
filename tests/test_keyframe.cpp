#include "support/common.hpp"

using namespace testsupport;
using stills::Time;
using stills::Tolerance;

TEST_CASE("tolerance: any() returns the keyframe at or before the time", "[sync][keyframe]") {
  stills::Options o = sw_options();
  o.tolerance = Tolerance::any();
  auto g = open_counter(o);
  for (int n : {0, 15, 29, 30, 45, 89, 119}) {
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    const int expected = (n / 30) * 30;
    INFO("request " << n);
    CHECK(frame_index_of(img) == expected);
    CHECK(img.actual_time() == Time{expected, 30});
    CHECK(img.is_keyframe());
  }
}

TEST_CASE("tolerance: a finite window stops decoding at the first acceptable frame",
          "[sync][keyframe]") {
  SECTION("before-tolerance covering the keyframe") {
    stills::Options o = sw_options();
    o.tolerance = Tolerance{Time{1, 1}, Time::zero()};  // up to 1 s early is fine
    auto g = open_counter(o);
    auto img = REQUIRE_OK(g.image_at(Time{47, 30}));
    CHECK(frame_index_of(img) == 30);  // keyframe 30 is within 1 s before 47 and comes first
    CHECK(img.is_keyframe());
    img = REQUIRE_OK(g.image_at(Time{100, 30}));
    CHECK(frame_index_of(img) == 90);
  }
  SECTION("before-tolerance too small to reach the keyframe") {
    stills::Options o = sw_options();
    o.tolerance = Tolerance{Time{5, 30}, Time::zero()};  // 5 frames early is fine
    auto g = open_counter(o);
    auto img = REQUIRE_OK(g.image_at(Time{47, 30}));
    CHECK(frame_index_of(img) == 42);  // first decoded frame inside [42, 47]
  }
  SECTION("after-tolerance never replaces an already decoded exact frame") {
    stills::Options o = sw_options();
    o.tolerance = Tolerance{Time::zero(), Time{1, 1}};
    auto g = open_counter(o);
    auto img = REQUIRE_OK(g.image_at(Time{47, 30}));
    CHECK(frame_index_of(img) == 47);
  }
  SECTION("negative tolerances are rejected at open") {
    stills::Options o = sw_options();
    o.tolerance = Tolerance{Time{-1, 1}, Time::zero()};
    REQUIRE_ERROR(stills::AssetImageGenerator::open(fixture("counter.mp4").string(), o),
                  stills::ErrorCode::invalid_argument);
  }
  SECTION("clamp_to_last_frame with any() returns the last keyframe, flagged") {
    stills::Options o = sw_options();
    o.tolerance = Tolerance::any();
    o.out_of_range = stills::OutOfRangePolicy::clamp_to_last_frame;
    auto g = open_counter(o);
    auto img = REQUIRE_OK(g.image_at(Time{10, 1}));
    CHECK(frame_index_of(img) == 90);
    CHECK(img.is_keyframe());
    CHECK(img.was_clamped());
  }
}

// Keyframe mode on an edit-list source. The keyframe covering the first presented second (source
// frame 30) precedes the edit and is never output, so requests there clamp to the first presented
// frame; the landing must be verified against the index (the mov seek lands one GOP early just
// after keyframe 90, at 57/30 s).
TEST_CASE(
    "tolerance: any() on an edit-list source clamps to the first presented frame and finds the "
    "last keyframe",
    "[sync][keyframe][editlist]") {
  stills::Options o = sw_options();
  o.tolerance = Tolerance::any();
  auto g =
      REQUIRE_OK(stills::AssetImageGenerator::open(fixture("counter_editlist.mp4").string(), o));
  struct Case {
    int n;
    int expected;
    bool clamped;
  };
  // presented: source 33..119 at 0..86/30 s; keyframes 60 at 27/30 s and 90 at 57/30 s
  const Case cases[] = {{0, 33, true},   {10, 33, true},  {26, 33, true},  {27, 60, false},
                        {28, 60, false}, {40, 60, false}, {56, 60, false}, {57, 90, false},
                        {58, 90, false}, {86, 90, false}, {20, 33, true},  {57, 90, false},
                        {5, 33, true},   {59, 90, false}, {27, 60, false}, {0, 33, true}};
  for (const Case& c : cases) {
    auto img = REQUIRE_OK(g.image_at(Time{c.n, 30}));
    INFO("t = " << c.n << "/30: got " << frame_index_of(img) << " at " << img.actual_time()
                << " key " << img.is_keyframe() << " clamped " << img.was_clamped());
    CHECK(frame_index_of(img) == c.expected);
    CHECK(img.was_clamped() == c.clamped);
    if (!c.clamped) CHECK(img.is_keyframe());
    CHECK(img.actual_time() == Time{c.expected - 33, 30});
  }
}

// Nearest-keyframe mode answers with the keyframe at or before the request without decoding
// anything after it, so on a file whose data stops earlier than its container claims it used to
// hand back the last keyframe unflagged, six seconds past the end, where exact mode reports
// time_out_of_range. The tail is verified by reading packets once per source.
TEST_CASE("keyframe: past the end of a truncated file the out-of-range policy applies",
          "[keyframe][bounds]") {
  const auto path = fixture("counter_trunc.mp4").string();
  stills::Options any = sw_options();
  any.tolerance = stills::Tolerance::any();
  SECTION("error policy") {
    auto g = REQUIRE_OK(stills::AssetImageGenerator::open(path, any));
    // Somewhere inside the data the keyframe answer is still the keyframe answer.
    auto early = REQUIRE_OK(g.image_at(Time{10, 30}));
    CHECK(early.is_keyframe());
    CHECK_FALSE(early.was_clamped());
    // Past the data, twice: the second must answer like the first.
    auto first = g.image_at(Time{115, 30});
    INFO("first: " << (first ? std::string{"image"} : to_string(first.error())));
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().code == stills::ErrorCode::time_out_of_range);
    auto again = g.image_at(Time{115, 30});
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == stills::ErrorCode::time_out_of_range);
    // Exact mode agrees, which is the point: the two modes must not disagree about the end.
    REQUIRE_ERROR(
        g.image_at(Time{115, 30}, stills::RequestOptions{.tolerance = stills::Tolerance::exact()}),
        stills::ErrorCode::time_out_of_range);
  }
  SECTION("clamp policy flags the frame") {
    stills::Options clamp = any;
    clamp.out_of_range = stills::OutOfRangePolicy::clamp_to_last_frame;
    auto g = REQUIRE_OK(stills::AssetImageGenerator::open(path, clamp));
    auto img = REQUIRE_OK(g.image_at(Time{115, 30}));
    CHECK(img.was_clamped());
  }
}

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

namespace {
void expect_exact(AssetImageGenerator& g, int n) {
  INFO("request " << n);
  auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
  CHECK(frame_index_of(img) == n);
  CHECK_FALSE(img.was_clamped());
  CHECK_FALSE(img.is_corrupt());
}
}  // namespace

TEST_CASE("containers: MPEG-TS backward and cross-GOP requests are exact", "[sync][ts]") {
  const stills::HardwarePolicy policy =
      GENERATE(stills::HardwarePolicy::software_only, stills::HardwarePolicy::prefer_hardware);
  stills::Options o = sw_options();
  o.hardware.policy = policy;
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_offset.ts").string(), o));
  INFO("decoder: " << (g.active_decoder().hardware ? "hardware" : "software"));
  CHECK(g.info().container_name == "mpegts");
  CHECK(g.info().seekable);
  SECTION("into the first GOP after moving past it") {
    for (int n : {45, 1, 0, 29, 2, 5}) expect_exact(g, n);
  }
  SECTION("into the last GOP, where no keyframe follows the landing point") {
    for (int n : {100, 119, 118, 90, 89, 115}) expect_exact(g, n);
  }
  SECTION("random permutation") {
    std::vector<int> order(120);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937{7});
    for (int n : order) expect_exact(g, n);
  }
  SECTION("backward one frame at a time") {
    for (int n = 119; n >= 80; --n) expect_exact(g, n);
  }
  SECTION("times before the first frame clamp to it, not to the next keyframe") {
    auto late = REQUIRE_OK(g.image_at(Time{70, 30}));
    CHECK(frame_index_of(late) == 70);
    auto first = REQUIRE_OK(g.image_at(Time::zero()));
    CHECK(frame_index_of(first) == 0);
    CHECK(first.actual_time() == Time::zero());
    CHECK_FALSE(first.was_clamped());
  }
  SECTION("keyframe mode returns the keyframe at or before the time") {
    stills::Options k = o;
    k.tolerance = stills::Tolerance::any();
    auto kg = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_offset.ts").string(), k));
    for (int n : {45, 15, 119, 0, 89, 90}) {
      auto img = REQUIRE_OK(kg.image_at(Time{n, 30}));
      INFO("request " << n);
      CHECK(frame_index_of(img) == (n / 30) * 30);
      CHECK(img.is_keyframe());
    }
  }
}

TEST_CASE("containers: MPEG-TS with a single GOP needs the explicit start seek", "[sync][ts]") {
  auto g =
      REQUIRE_OK(AssetImageGenerator::open(fixture("counter_longgop.ts").string(), sw_options()));
  for (int n : {45, 1, 100, 119, 60, 0, 31, 30, 89}) expect_exact(g, n);
  stills::Options o = sw_options();
  o.tolerance = stills::Tolerance::any();
  auto k = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_longgop.ts").string(), o));
  auto img = REQUIRE_OK(k.image_at(Time{100, 30}));
  CHECK(frame_index_of(img) == 0);  // the only keyframe
  CHECK(img.is_keyframe());
}

TEST_CASE("containers: Matroska exact frame times return frame k", "[sync][mkv]") {
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mkv").string(), sw_options()));
  CHECK(g.info().container_name.find("matroska") != std::string::npos);
  CHECK(g.info().time_base == stills::Rational{1, 1000});
  for (int n : {29, 5, 2, 119, 89, 0, 1, 30, 31, 60, 118, 45}) {
    INFO("request " << n);
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    CHECK(frame_index_of(img) == n);
    // actual_time() is the container's millisecond-rounded value: within half a tick of k/30.
    const double diff = std::abs(img.actual_time().to_seconds() - n / 30.0);
    CHECK(diff <= 0.0005 + 1e-9);
    CHECK_FALSE(img.was_clamped());
  }
  // Midpoints still resolve to the earlier frame.
  for (int n : {0, 14, 29, 77}) {
    auto img = REQUIRE_OK(g.image_at(Time{2 * n + 1, 60}));
    CHECK(frame_index_of(img) == n);
  }
  // Time::frames() is the canonical way to ask for frame k.
  auto f = REQUIRE_OK(g.image_at(Time::frames(59, {30, 1})));
  CHECK(frame_index_of(f) == 59);
}

TEST_CASE("containers: WebM/VP9 exact frame times return frame k", "[sync][mkv]") {
#if !STILLS_HAVE_VP9
  SKIP("ffmpeg was built without libvpx-vp9; fixture not generated");
#else
  auto g =
      REQUIRE_OK(AssetImageGenerator::open(fixture("counter_vp9.webm").string(), sw_options()));
  CHECK(g.info().codec_name.find("vp9") != std::string::npos);
  for (int n : {119, 29, 5, 2, 89, 0, 60, 45}) {
    INFO("request " << n);
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    CHECK(frame_index_of(img) == n);
  }
#endif
}

TEST_CASE("containers: edit list — time zero is the first presented frame", "[sync][editlist]") {
  auto g =
      REQUIRE_OK(AssetImageGenerator::open(fixture("counter_editlist.mp4").string(), sw_options()));
  auto first = REQUIRE_OK(g.image_at(Time::zero()));
  const int f0 = frame_index_of(first);
  CHECK(first.actual_time() == Time::zero());
  CHECK(f0 >= 30);  // the copy started at 1.1 s: the keyframe at 1.0 s is trimmed by the edit list
  CHECK_FALSE(first.is_keyframe());
  for (int k : {1, 10, 50, 3, 80}) {
    INFO("request " << k);
    auto img = REQUIRE_OK(g.image_at(Time{k, 30}));
    CHECK(frame_index_of(img) == f0 + k);
    CHECK(img.actual_time() == Time{k, 30});
  }
}

TEST_CASE("containers: the video stream is not stream 0", "[open]") {
  auto g =
      REQUIRE_OK(AssetImageGenerator::open(fixture("av_audio_first.mp4").string(), sw_options()));
  CHECK(g.info().video_stream_index == 1);
  for (int n : {0, 20, 7}) expect_exact(g, n);  // -shortest: one second of video
  stills::Options o = sw_options();
  o.video_stream_index = 0;  // the audio stream
  REQUIRE_ERROR(AssetImageGenerator::open(fixture("av_audio_first.mp4").string(), o),
                ErrorCode::invalid_argument);
  o.video_stream_index = 1;
  auto explicit_video =
      REQUIRE_OK(AssetImageGenerator::open(fixture("av_audio_first.mp4").string(), o));
  expect_exact(explicit_video, 12);
}

TEST_CASE("containers: sync results on every container match the MP4 reference",
          "[sync][accuracy]") {
  const char* file = GENERATE("counter_offset.ts", "counter.mkv", "counter_longgop.ts");
  auto g = REQUIRE_OK(
      AssetImageGenerator::open(fixture(file).string(), sw_options(stills::PixelFormat::rgba)));
  auto ref = open_counter(sw_options(stills::PixelFormat::rgba));
  for (int n : {3, 90, 31, 119, 0, 64}) {
    INFO(file << " frame " << n);
    auto a = REQUIRE_OK(g.image_at(Time{n, 30}));
    auto b = REQUIRE_OK(ref.image_at(Time{n, 30}));
    CHECK(frame_index_of(a) == n);
    CHECK(a.size() == b.size());
    if (std::string_view{file} != "counter_longgop.ts") CHECK(a.is_keyframe() == b.is_keyframe());
  }
}

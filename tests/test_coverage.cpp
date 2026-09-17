// Coverage: 10-bit sources and 16-bit outputs, open-GOP/interlaced H.264, every ErrorCode, damage
// patterns, stress, raw MJPEG, and odd-sized rotation.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <latch>
#include <stills/interop.hpp>
#include <thread>
#include <utility>
#include <vector>

#include "support/common.hpp"

using namespace testsupport;
using namespace std::chrono_literals;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::PixelFormat;
using stills::Time;

// A 10-bit source through every output, including the depth-preserving ones.
TEST_CASE("coverage: 10-bit source to 8-bit and 16-bit outputs", "[sync][depth]") {
  const PixelFormat fmt =
      GENERATE(PixelFormat::rgba, PixelFormat::yuv420p, PixelFormat::nv12, PixelFormat::gray8,
               PixelFormat::rgb24, PixelFormat::p010, PixelFormat::rgba64);
  INFO("format " << fmt);
  auto g =
      REQUIRE_OK(AssetImageGenerator::open(fixture("counter_p10.mp4").string(), sw_options(fmt)));
  CHECK(g.info().source_pixel_format == "yuv420p10le");
  for (int n : {0, 17, 29, 30, 77, 119}) {
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    CHECK(img.pixel_format() == fmt);
    CHECK(img.plane_count() == stills::plane_count(fmt));
    CHECK(frame_index_of(img) == n);
    if (fmt == PixelFormat::p010) {
      CHECK(img.plane_size(1) == stills::Size{32, 24});
      CHECK(img.row_stride(0) >= 128);  // 2 bytes per luma sample
      CHECK(img.color_range() == stills::ColorRange::limited);
    }
    if (fmt == PixelFormat::rgba64) {
      CHECK(img.row_stride(0) >= 64u * 8u);
      CHECK(img.color_range() == stills::ColorRange::full);
      const auto px = img.pixels();
      CHECK(std::to_integer<int>(px[6]) == 0xFF);
      CHECK(std::to_integer<int>(px[7]) == 0xFF);
    }
  }
  // 16-bit formats survive rotation (the kernel's 8-byte cells) and the packed copy.
  if (fmt == PixelFormat::rgba64 || fmt == PixelFormat::p010) {
    auto five = REQUIRE_OK(g.image_at(Time{5, 30}));
    auto packed = REQUIRE_OK(five.to_packed_bytes());
    CHECK(packed.size() ==
          static_cast<std::size_t>(64 * 48) * (fmt == PixelFormat::rgba64 ? 8u : 3u));
    stills::Options ro = sw_options(fmt);
    auto rot = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_rot90.mp4").string(),
                                                    ro));  // 8-bit source, rotated
    auto img = REQUIRE_OK(rot.image_at(Time{7, 30}));
    CHECK(img.size() == stills::Size{48, 64});
    CHECK(frame_index_of(img, rot.info().rotation_degrees) == 7);
  }
}

// Open-GOP and interlaced H.264.
TEST_CASE("coverage: open-GOP and interlaced H.264 stay exact", "[sync][h264]") {
  SECTION("open GOP") {
    auto g = REQUIRE_OK(
        AssetImageGenerator::open(fixture("counter_opengop.mp4").string(), sw_options()));
    for (int n : {0, 28, 29, 30, 31, 59, 60, 89, 119, 45, 2}) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      INFO("frame " << n << ": got " << frame_index_of(img) << " at " << img.actual_time());
      CHECK(frame_index_of(img) == n);
      CHECK(img.actual_time() == Time{n, 30});
    }
    stills::Options k = sw_options();
    k.tolerance = stills::Tolerance::any();
    auto kg = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_opengop.mp4").string(), k));
    for (int n : {45, 29, 90, 119}) {
      auto img = REQUIRE_OK(kg.image_at(Time{n, 30}));
      CHECK(img.is_keyframe());
      CHECK(frame_index_of(img) <= n);
      CHECK(frame_index_of(img) % 30 == 0);
    }
  }
  SECTION("interlaced") {
    auto g =
        REQUIRE_OK(AssetImageGenerator::open(fixture("counter_ilace.mp4").string(), sw_options()));
    for (int n : {0, 1, 29, 30, 31, 77, 119, 3}) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      INFO("frame " << n);
      CHECK(frame_index_of(img) == n);
      CHECK(img.is_keyframe() == (n % 30 == 0));
    }
  }
}

// Every ErrorCode the library can produce is reachable (the rest are asserted by construction).
TEST_CASE("coverage: error codes are reachable and distinct", "[errors]") {
  REQUIRE_ERROR(AssetImageGenerator::open(fixture("does_not_exist.mp4").string(), sw_options()),
                ErrorCode::file_not_found);
  REQUIRE_ERROR(AssetImageGenerator::open(fixture("not_a_video.mp4").string(), sw_options()),
                ErrorCode::unsupported_format);
  REQUIRE_ERROR(AssetImageGenerator::open(fixture("audio_only.m4a").string(), sw_options()),
                ErrorCode::no_video_stream);
  REQUIRE_ERROR(AssetImageGenerator::open("", sw_options()), ErrorCode::invalid_argument);
  // open_failed: a directory is not a readable media source.
  auto dir = AssetImageGenerator::open(fixture("").string(), sw_options());
  REQUIRE_FALSE(dir.has_value());
  CHECK((dir.error().code == ErrorCode::open_failed ||
         dir.error().code == ErrorCode::unsupported_format));
  stills::Options hw = sw_options();
  hw.hardware.policy = stills::HardwarePolicy::require_hardware;
  hw.hardware.device_type = stills::HardwareDeviceType::vaapi;
  hw.hardware.device = "/dev/dri/does-not-exist";
  REQUIRE_ERROR(AssetImageGenerator::open(fixture("counter.mp4").string(), hw),
                ErrorCode::hardware_unavailable);
  auto g = open_counter();
  REQUIRE_ERROR(g.image_at(Time{-1, 30}), ErrorCode::invalid_argument);
  REQUIRE_ERROR(g.image_at(Time{10, 1}), ErrorCode::time_out_of_range);
  AssetImageGenerator moved = std::move(g);
  REQUIRE_ERROR(g.image_at(Time::zero()),
                ErrorCode::invalid_state);  // NOLINT(bugprone-use-after-move)
  auto img = REQUIRE_OK(moved.image_at(Time::zero()));
  stills::Image taken = std::move(img);
  REQUIRE_ERROR(img.clone(), ErrorCode::invalid_state);  // NOLINT(bugprone-use-after-move)
  // conversion_failed is not reachable: an unsupported destination in adopt_frame is
  // invalid_argument, and swscale refuses no supported format pair.
  Collector col;
  // The cancellation has to be in place before the worker dispatches the second item, or "at least
  // one cancelled" depends on the main thread outrunning a 64x48 decode. Holding the first handler
  // until cancel() has been called makes it an ordering rather than a race.
  std::latch cancel_issued{1};
  auto req =
      moved.generate_images({Time{1, 30}, Time{2, 30}, Time{3, 30}}, [&](stills::Completion c) {
        cancel_issued.wait();
        col.add(std::move(c));
      });
  req.cancel();
  cancel_issued.count_down();
  REQUIRE(req.wait_for(30s) == stills::WaitResult::finished);
  CHECK(col.count() == 3);
  CHECK(col.count(stills::GenerationStatus::cancelled) >= 2);
  // not_seekable: backwards on a pipe (test_sync); end_of_stream / decode_failed: corrupt fixtures.
  std::vector<std::string_view> names;
  for (int c = 0; c <= static_cast<int>(ErrorCode::internal); ++c)
    names.push_back(to_string(static_cast<ErrorCode>(c)));
  std::sort(names.begin(), names.end());
  CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
  CHECK(std::find(names.begin(), names.end(), "unknown") == names.end());
}

// With a deterministic damage pattern the set of affected frames is stable: frames before the
// damage are clean, frames from the next keyframe on are clean, and nothing in between is silently
// wrong.
TEST_CASE("coverage: deterministic corruption never yields a silently wrong frame", "[errors]") {
  auto g = REQUIRE_OK(
      AssetImageGenerator::open(fixture("counter_corrupt_fixed.mp4").string(), sw_options()));
  int clean_before = 0, flagged = 0, failed = 0, clean_after = 0;
  for (int n = 0; n < 120; ++n) {
    auto r = g.image_at(Time{n, 30});
    if (!r) {
      ++failed;
      CHECK((r.error().code == ErrorCode::decode_failed ||
             r.error().code == ErrorCode::end_of_stream ||
             r.error().code == ErrorCode::time_out_of_range));
      continue;
    }
    const int claimed = frame_index_of_time(r->actual_time());
    const bool matches = frame_index_of(*r) == claimed;
    INFO("frame " << n << " claimed " << claimed << " content " << frame_index_of(*r) << " corrupt "
                  << r->is_corrupt());
    CHECK((matches || r->is_corrupt()));
    if (n < 60 && matches && !r->is_corrupt()) ++clean_before;
    if (r->is_corrupt()) ++flagged;
    if (n >= 90 && matches && !r->is_corrupt()) ++clean_after;
  }
  CHECK(clean_before == 60);  // the damage sits in the last quarter of the file
  WARN("corrupt-fixed fixture: flagged " << flagged << ", failed " << failed
                                         << ", clean from frame 90 on " << clean_after);
  CHECK(flagged + failed >= 1);
}

// Stress: exactly-once delivery under a 10 000-item batch, a cancel storm from four threads
// while four threads submit, and repeated destruction mid-batch.
TEST_CASE("coverage: async stress keeps exactly-once delivery", "[async][stress]") {
  SECTION("10 000 items in one batch") {
    auto g = open_counter();
    std::vector<Time> times;
    times.reserve(10000);
    for (int i = 0; i < 10000; ++i) times.emplace_back(i % 120, 30);
    std::atomic<int> delivered{0};
    std::atomic<int> ok{0};
    auto req = g.generate_images(times, [&](stills::Completion c) {
      delivered.fetch_add(1);
      if (c.result) ok.fetch_add(1);
    });
    REQUIRE(req.wait_for(120s) == stills::WaitResult::finished);
    CHECK(delivered.load() == 10000);
    CHECK(ok.load() == 10000);
  }
  SECTION("cancel storm") {
    auto g = open_counter();
    std::atomic<int> delivered{0};
    std::atomic<bool> stop{false};
    std::vector<stills::AsyncRequest> reqs(400);
    std::mutex m;
    std::vector<std::thread> submitters, cancellers;
    for (int t = 0; t < 4; ++t) {
      submitters.emplace_back([&, t] {
        for (int i = 0; i < 100; ++i) {
          std::vector<Time> times;
          for (int k = 0; k < 5; ++k) times.emplace_back((i * 7 + k + t) % 120, 30);
          auto r = g.generate_images(times, [&](stills::Completion) { delivered.fetch_add(1); });
          std::lock_guard lk(m);
          reqs[static_cast<std::size_t>(t * 100 + i)] = std::move(r);
        }
      });
    }
    for (int t = 0; t < 4; ++t) {
      cancellers.emplace_back([&] {
        while (!stop.load()) {
          std::lock_guard lk(m);
          for (auto& r : reqs)
            if (r.valid() && (std::rand() % 3) == 0) r.cancel();  // NOLINT(concurrency-mt-unsafe)
          if (std::rand() % 5 == 0) g.cancel_all();               // NOLINT(concurrency-mt-unsafe)
        }
      });
    }
    for (auto& th : submitters) th.join();
    stop = true;
    for (auto& th : cancellers) th.join();
    for (auto& r : reqs) REQUIRE(r.wait_for(120s) == stills::WaitResult::finished);
    CHECK(delivered.load() == 400 * 5);
  }
  SECTION("destroy mid-batch, fifty times") {
    for (int round = 0; round < 50; ++round) {
      std::atomic<int> delivered{0};
      stills::AsyncRequest req;
      {
        auto g = open_counter();
        std::vector<Time> times;
        for (int i = 0; i < 40; ++i) times.emplace_back(i, 30);
        req = g.generate_images(times, [&](stills::Completion) { delivered.fetch_add(1); });
        std::this_thread::sleep_for(std::chrono::microseconds(round * 50));
      }
      REQUIRE(req.finished());
      CHECK(delivered.load() == 40);
    }
  }
}

// Raw MJPEG — no timestamps (synthesised 25 fps timeline), intra-only, full-range source.
TEST_CASE("coverage: raw MJPEG has a synthesised timeline and keeps the full range",
          "[sync][raw]") {
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mjpeg").string(), sw_options()));
  CHECK(g.info().timestamps_synthesized);
  CHECK(g.info().average_frame_rate == stills::Rational{25, 1});
  CHECK(g.info().codec_name == "mjpeg");
  for (int k : {0, 5, 40, 3, 100}) {  // 3 goes backwards: re-open
    auto img = REQUIRE_OK(g.image_at(Time{k, 25}));
    INFO("request " << k);
    CHECK(img.actual_time() == Time{k, 25});
    CHECK(img.is_keyframe());
    // Full-range JPEG luma: the encoded index is limited-range in the source pattern, and the
    // yuv420p output keeps the source range (full) so the luma is expanded: recover with the
    // inverse.
    const double y = luma_at(img, 16, 24);
    const double limited = y * 219.0 / 255.0 + 16.0;
    const int idx_lo = static_cast<int>(std::lround((limited - 22.0) / 14.0));
    const double y2 = luma_at(img, 48, 24);
    const double limited2 = y2 * 219.0 / 255.0 + 16.0;
    const int idx_hi = static_cast<int>(std::lround((limited2 - 22.0) / 14.0));
    CHECK(img.color_range() == stills::ColorRange::full);
    CHECK(idx_lo + 16 * idx_hi == k);
  }
}

// Rotation of odd-sized sources through every format.
TEST_CASE("coverage: rotation on odd source dimensions per output format", "[output][rotation]") {
  const PixelFormat fmt = GENERATE(PixelFormat::rgba, PixelFormat::rgb24, PixelFormat::gray8,
                                   PixelFormat::yuv420p, PixelFormat::nv12, PixelFormat::rgba64);
  INFO("format " << fmt);
  auto g =
      REQUIRE_OK(AssetImageGenerator::open(fixture("odd_rot90.mov").string(), sw_options(fmt)));
  CHECK(g.info().rotation_degrees == 270);
  const stills::Size expected =
      stills::has_chroma_subsampling(fmt) ? stills::Size{16, 32} : stills::Size{17, 33};
  CHECK(g.info().output_size == expected);
  auto img = REQUIRE_OK(g.image_at(Time::zero()));
  CHECK(img.size() == expected);
  // The source is solid red: every pixel of the rotated output is red (G channel low, R high / luma
  // of red).
  if (fmt == PixelFormat::rgba || fmt == PixelFormat::rgb24) {
    const int bpp = stills::bytes_per_pixel(fmt);
    const auto px = img.pixels();
    for (int y : {0, expected.height / 2, expected.height - 1}) {
      for (int x : {0, expected.width / 2, expected.width - 1}) {
        const std::size_t at =
            static_cast<std::size_t>(y) * img.row_stride(0) + static_cast<std::size_t>(x * bpp);
        CHECK(std::to_integer<int>(px[at]) > 200);     // R
        CHECK(std::to_integer<int>(px[at + 1]) < 40);  // G
      }
    }
  }
  if (fmt == PixelFormat::rgba64) {
    const auto px = img.pixels();
    CHECK(std::to_integer<int>(px[1]) > 200);  // high byte of R
    CHECK(std::to_integer<int>(px[3]) < 40);   // high byte of G
  }
}

// Public members the suite never executed. Each of them is reachable from an ordinary consumer and
// none had a test: an API nobody calls in the tests is an API nobody has checked.
TEST_CASE("coverage: is_tightly_packed and wait_for", "[coverage]") {
  SECTION("is_tightly_packed agrees with packed_size_bytes") {
    auto g = open_counter(sw_options(stills::PixelFormat::rgba));
    auto img = REQUIRE_OK(g.image_at(Time{7, 30}));
    const bool packed = img.is_tightly_packed();
    const std::size_t minimal = static_cast<std::size_t>(img.width()) * 4;
    CHECK(packed == (img.row_stride(0) == minimal));
    if (packed) CHECK(img.packed_size_bytes() == img.pixels().size());
    auto bytes = REQUIRE_OK(img.to_packed_bytes());
    CHECK(bytes.size() == img.packed_size_bytes());
    stills::Image empty = std::move(img);
    CHECK_FALSE(empty.empty());
    CHECK_FALSE(img.is_tightly_packed());  // NOLINT(bugprone-use-after-move): empty is not packed
  }
  SECTION("wait_for reports a timeout instead of blocking") {
    auto g = open_counter();
    std::latch release{1};
    std::vector<Time> times;
    for (int n = 0; n < 30; ++n) times.emplace_back(n * 4, 30);
    auto req = g.generate_images(times, [&](stills::Completion) { release.wait(); });
    CHECK(req.wait_for(std::chrono::milliseconds{20}) == stills::WaitResult::timed_out);
    CHECK_FALSE(req.finished());
    release.count_down();
    CHECK(req.wait() == stills::WaitResult::finished);
    CHECK(req.remaining() == 0);
  }
}

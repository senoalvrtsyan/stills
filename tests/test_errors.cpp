#include <limits>
#include <sstream>

#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::Time;

TEST_CASE("errors: truncated file", "[errors]") {
  auto g =
      REQUIRE_OK(AssetImageGenerator::open(fixture("counter_trunc.mp4").string(), sw_options()));
  auto early = REQUIRE_OK(g.image_at(Time{5, 30}));
  CHECK(frame_index_of(early) == 5);
  auto late = g.image_at(Time{110, 30});
  REQUIRE_FALSE(late.has_value());
  INFO("late error: " << late.error());
  CHECK((late.error().code == ErrorCode::time_out_of_range ||
         late.error().code == ErrorCode::end_of_stream ||
         late.error().code == ErrorCode::decode_failed));
  // The generator stays usable after a per-request failure.
  auto again = REQUIRE_OK(g.image_at(Time{7, 30}));
  CHECK(frame_index_of(again) == 7);
}

TEST_CASE("errors: corrupt data never yields a silently wrong frame", "[errors]") {
  auto g =
      REQUIRE_OK(AssetImageGenerator::open(fixture("counter_corrupt.mp4").string(), sw_options()));
  for (int n = 0; n < 120; n += 7) {
    auto r = g.image_at(Time{n, 30});
    if (!r) {
      INFO("frame " << n << ": " << r.error());
      CHECK((r.error().code == ErrorCode::decode_failed ||
             r.error().code == ErrorCode::end_of_stream ||
             r.error().code == ErrorCode::time_out_of_range));
      continue;
    }
    // The content must match the frame the library claims to have returned, unless flagged corrupt.
    const int claimed = frame_index_of_time(r->actual_time());
    INFO("frame " << n << " claimed " << claimed << " content " << frame_index_of(*r) << " corrupt "
                  << r->is_corrupt());
    CHECK((frame_index_of(*r) == claimed || r->is_corrupt()));
    CHECK(r->actual_time() <= Time{n, 30});
  }
  // Frames before the damage are exact.
  for (int n : {0, 10, 20}) {
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    CHECK(frame_index_of(img) == n);
    CHECK_FALSE(img.is_corrupt());
  }
}

TEST_CASE("errors: Error rendering", "[errors]") {
  const stills::Error e{ErrorCode::open_failed, AVERROR(EACCES),
                        "avformat_open_input(\"x\"): Permission denied"};
  const std::string s = to_string(e);
  CHECK(s.find("open_failed") != std::string::npos);
  CHECK(s.find("Permission denied") != std::string::npos);
  CHECK(s.find("AVERROR") != std::string::npos);
  CHECK(to_string(stills::Error{ErrorCode::cancelled}) == "cancelled");
  std::ostringstream os;
  os << ErrorCode::no_video_stream;
  CHECK(os.str() == "no_video_stream");
#if defined(__cpp_lib_format)
  CHECK(std::format("{}", ErrorCode::decode_failed) == "decode_failed");
#endif
}

// Two signed additions in the request path could overflow: the requested time plus the stream's
// time origin (an MPEG-TS starting at 10 s), and the keyframe-mode scan horizon, which is handed
// INT64_MAX for "the next keyframe, wherever it is". Under GCC's wrap both failed as intended;
// under a consumer's UBSan, or this project's asan preset (-fno-sanitize-recover=all), the process
// aborted instead of returning an error. Both are checked here, so the asan preset covers them.
TEST_CASE("errors: huge times on an offset stream do not overflow", "[errors][overflow]") {
  auto g = REQUIRE_OK(
      stills::AssetImageGenerator::open(fixture("counter_offset.ts").string(), sw_options()));
  const Time huge[] = {
      Time{std::numeric_limits<std::int64_t>::max(), 1},
      Time{std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int32_t>::max()},
      Time{std::numeric_limits<std::int64_t>::max() / 2, 1},
      Time{std::numeric_limits<std::int64_t>::max() / 90000, 1},
  };
  for (const Time t : huge) {
    auto img = g.image_at(t);
    INFO("exact " << t);
    REQUIRE_FALSE(img.has_value());
    CHECK((img.error().code == stills::ErrorCode::time_out_of_range ||
           img.error().code == stills::ErrorCode::invalid_argument));
  }
  // Keyframe mode, after a byte seek has positioned the scanned index: the horizon is INT64_MAX.
  stills::RequestOptions any;
  any.tolerance = stills::Tolerance::any();
  (void)REQUIRE_OK(g.image_at(Time{90, 30}, any));
  (void)REQUIRE_OK(g.image_at(Time{40, 30}, any));
  (void)REQUIRE_OK(g.image_at(Time{95, 30}, any));
  for (const Time t : huge) {
    auto img = g.image_at(t, any);
    INFO("keyframe " << t);
    REQUIRE_FALSE(img.has_value());
  }
  // The generator is still usable afterwards.
  auto ok = REQUIRE_OK(g.image_at(Time{7, 30}));
  CHECK(frame_index_of(ok) == 7);
}

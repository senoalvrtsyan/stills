// Options::validate(), interop preconditions, Time conveniences.
#include <chrono>
#include <limits>
#include <stills/interop.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::Options;
using stills::Time;

TEST_CASE("options: validate() rejects every documented misuse", "[open][options]") {
  const auto rejects = [](Options o, const char* what) {
    INFO(what);
    auto v = o.validate();
    REQUIRE_FALSE(v.has_value());
    CHECK(v.error().code == ErrorCode::invalid_argument);
    CHECK_FALSE(v.error().message.empty());
    REQUIRE_ERROR(AssetImageGenerator::open(fixture("counter.mp4").string(), o),
                  ErrorCode::invalid_argument);
  };
  Options o = sw_options();
  REQUIRE_OK(o.validate());

  o = sw_options();
  o.tolerance.before = Time{-1, 30};
  rejects(o, "negative before");
  o = sw_options();
  o.tolerance.after = Time{-1, 1000};
  rejects(o, "negative after");
  o = sw_options();
  o.tolerance.before = Time::negative_infinity();
  rejects(o, "-inf");
  o = sw_options();
  o.tolerance.after = Time::invalid();
  rejects(o, "invalid");
  o = sw_options();
  o.video_stream_index = -5;
  rejects(o, "negative stream index");
  o = sw_options();
  o.maximum_size = stills::Size{-1, 0};
  rejects(o, "negative box");
  o = sw_options();
  o.decoder_threads = -1;
  rejects(o, "negative threads");
  o = sw_options(stills::PixelFormat::yuv420p);
  o.maximum_size = stills::Size{1, 1};
  rejects(o, "1x1 box for 4:2:0");
  o = sw_options(stills::PixelFormat::nv12);
  o.maximum_size = stills::Size{0, 1};
  rejects(o, "height 1 for nv12");
  o = sw_options();
  o.hardware.device_type = stills::HardwareDeviceType::vaapi;
  rejects(o, "device type with software_only");
  o = sw_options();
  o.hardware.device = "/dev/dri/renderD128";
  rejects(o, "device with software_only");
  o = Options{};
  o.hardware.device = "/dev/dri/renderD128";
  rejects(o, "device without a type");
  o = sw_options();
  o.demuxer_options = {{"", "x"}};
  rejects(o, "empty option key");

  // Accepted edge cases.
  o = sw_options(stills::PixelFormat::rgba);
  o.maximum_size = stills::Size{1, 1};
  REQUIRE_OK(o.validate());
  auto tiny = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mp4").string(), o));
  auto img = REQUIRE_OK(tiny.image_at(Time::zero()));
  CHECK(img.size() == stills::Size{1, 1});
  o = sw_options();
  o.tolerance = stills::Tolerance::any();
  REQUIRE_OK(o.validate());
  o = sw_options();
  o.demuxer_options = {{"probesize", "5000000"}};
  REQUIRE_OK(o.validate());
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mp4").string(), o));
  CHECK(frame_index_of(REQUIRE_OK(g.image_at(Time{9, 30}))) == 9);
}

TEST_CASE("options: filesystem::path overload", "[open]") {
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mp4"), sw_options()));
  CHECK(frame_index_of(REQUIRE_OK(g.image_at(Time{2, 30}))) == 2);
}

TEST_CASE("interop: adopt_frame preconditions", "[image][interop]") {
  // Accepts a properly allocated NV12 frame.
  AVFrame* nv12 = av_frame_alloc();
  nv12->format = AV_PIX_FMT_NV12;
  nv12->width = 8;
  nv12->height = 8;
  REQUIRE(av_frame_get_buffer(nv12, 0) == 0);
  nv12->pts = 3;
  auto ok = REQUIRE_OK(stills::interop::adopt_frame(nv12, stills::Rational{1, 30}));
  CHECK(ok.plane_count() == 2);
  CHECK(ok.plane_size(1) == stills::Size{4, 4});
  CHECK(ok.actual_time() == Time{3, 30});

  // Rejects a frame that does not own its pixels.
  std::vector<std::uint8_t> pixels(8 * 8 * 4);
  AVFrame* borrowed = av_frame_alloc();
  borrowed->format = AV_PIX_FMT_RGBA;
  borrowed->width = 8;
  borrowed->height = 8;
  borrowed->data[0] = pixels.data();
  borrowed->linesize[0] = 8 * 4;
  REQUIRE_ERROR(stills::interop::adopt_frame(borrowed), ErrorCode::invalid_argument);
  borrowed->data[0] = nullptr;
  av_frame_free(&borrowed);

  // Rejects a planar frame with a missing plane.
  AVFrame* missing = av_frame_alloc();
  missing->format = AV_PIX_FMT_YUV420P;
  missing->width = 8;
  missing->height = 8;
  REQUIRE(av_frame_get_buffer(missing, 0) == 0);
  std::uint8_t* saved = missing->data[2];
  missing->data[2] = nullptr;
  REQUIRE_ERROR(stills::interop::adopt_frame(missing), ErrorCode::invalid_argument);
  missing->data[2] = saved;
  av_frame_free(&missing);
}

TEST_CASE("time: conveniences", "[time]") {
  CHECK(Time::frames(29, {30, 1}) == Time{29, 30});
  CHECK(Time::frames(29, {30000, 1001}) == Time{29 * 1001, 30000});
  CHECK_FALSE(Time::frames(1, {0, 1}).is_valid());
  std::unordered_set<Time> set{Time{1, 2}, Time{2, 4}, Time{3, 6}, Time::zero(), Time{0, 30}};
  CHECK(set.size() == 2);  // 1/2 == 2/4 == 3/6; 0 == 0/30
  CHECK(std::hash<Time>{}(Time{1, 2}) == std::hash<Time>{}(Time{2, 4}));
  CHECK(std::hash<Time>{}(Time::positive_infinity()) !=
        std::hash<Time>{}(Time::negative_infinity()));
#if defined(__cpp_lib_format)
  CHECK(std::format("{}", Time{1, 2}) == "0.500000s (1/2)");
  CHECK(std::format("{}", stills::Rational{30, 1}) == "30/1");
  CHECK(std::format("{}", stills::Size{64, 48}) == "64x48");
#endif
  CHECK(to_string(stills::WaitResult::refused_on_worker_thread) == "refused_on_worker_thread");
}

// A container costs a few hundred bytes to declare an enormous frame, and open() allocates for it:
// the decoder is built and the first frame probed there, so a 8192x8192 declaration in a ~200 KB
// file reaches a gigabyte of resident memory before the caller can look at info().coded_size.
TEST_CASE("options: max_input_pixels refuses an oversized frame before the decoder is built",
          "[options][security]") {
  const auto path = fixture("huge8k.mp4").string();
  stills::Options capped = sw_options();
  capped.max_input_pixels = 8294400;  // 4K
  auto refused = stills::AssetImageGenerator::open(path, capped);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == stills::ErrorCode::unsupported_format);
  INFO(refused.error());
  CHECK(refused.error().message.find("8192x8192") != std::string::npos);
  CHECK(refused.error().message.find("max_input_pixels") != std::string::npos);

  // Without the cap the same file opens, which is why the cap has to be opt-in.
  auto allowed = stills::AssetImageGenerator::open(path, sw_options());
  REQUIRE(allowed.has_value());
  CHECK(allowed->info().coded_size == stills::Size{8192, 8192});

  // An ordinary clip is unaffected by a cap it fits under.
  auto ok = stills::AssetImageGenerator::open(fixture("counter.mp4").string(), capped);
  REQUIRE(ok.has_value());

  // The cap itself is validated.
  stills::Options bad = sw_options();
  bad.max_input_pixels = 0;
  REQUIRE_ERROR(stills::AssetImageGenerator::open(fixture("counter.mp4").string(), bad),
                stills::ErrorCode::invalid_argument);
}

// The source string reaches every protocol libavformat has, so a typo in a security setting must
// not be silent, and the whitelist keys documented on Options::demuxer_options have to work.
TEST_CASE("options: untrusted sources are confined to the file protocol", "[options][security]") {
  const auto clip = fixture("counter.mp4").string();
  SECTION("protocol_whitelist opens ordinary files and refuses concat:") {
    Options o = sw_options();
    o.demuxer_options = {{"protocol_whitelist", "file"},
                         {"format_whitelist", "mov,mp4,m4a,matroska,webm"}};
    auto g = stills::AssetImageGenerator::open(clip, o);
    REQUIRE(g.has_value());
    // `concat:` is a protocol, not a path: without a whitelist it opens whatever it is given,
    // which is how a path-prefix check on the source is defeated.
    auto allowed = stills::AssetImageGenerator::open("concat:" + clip, sw_options());
    CHECK(allowed.has_value());
    auto refused = stills::AssetImageGenerator::open("concat:" + clip, o);
    CHECK_FALSE(refused.has_value());
  }
  SECTION("a misspelled demuxer option is an error, not a silent no-op") {
    Options typo = sw_options();
    typo.demuxer_options = {{"protocol_whitlist", "file"}};
    auto r = stills::AssetImageGenerator::open(clip, typo);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::invalid_argument);
    INFO(r.error());
    CHECK(r.error().message.find("protocol_whitlist") != std::string::npos);
  }
  SECTION("a real key that this source does not use is accepted") {
    // HTTP options are real libavformat keys; a local path consumes none of them.
    Options http = sw_options();
    http.demuxer_options = {{"reconnect", "1"}, {"rw_timeout", "10000000"}};
    auto g = stills::AssetImageGenerator::open(clip, http);
    CHECK(g.has_value());
  }
}

// validate() used to check lower bounds only, so a typo like INT_MAX reached libavcodec and came
// back as "Cannot allocate memory" several calls later.
TEST_CASE("options: validate rejects an out-of-range decoder_threads", "[options]") {
  const auto clip = fixture("counter.mp4").string();
  const auto rejected = [&](auto tweak) {
    Options o = sw_options();
    tweak(o);
    auto r = AssetImageGenerator::open(clip, o);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::invalid_argument);
    INFO(r.error());
  };
  rejected([](Options& o) { o.decoder_threads = std::numeric_limits<int>::max(); });
  rejected([](Options& o) { o.decoder_threads = Options::max_decoder_threads + 1; });
  // The bounds themselves are accepted.
  Options ok = sw_options();
  ok.decoder_threads = 2;
  CHECK(AssetImageGenerator::open(clip, ok).has_value());
}

// ErrorCode separates "it is broken" from "position it first", and can travel as a
// std::error_code.
TEST_CASE("options: ErrorCode converts to std::error_code", "[errors]") {
  const std::error_code ec = stills::ErrorCode::time_out_of_range;
  CHECK(ec.category() == stills::error_category());
  CHECK(ec.category().name() == std::string{"stills"});
  CHECK(ec.message() == "time_out_of_range");
  CHECK(ec == stills::ErrorCode::time_out_of_range);
  CHECK(ec != stills::ErrorCode::invalid_state);
  CHECK(stills::make_error_code(stills::ErrorCode::invalid_state).value() ==
        static_cast<int>(stills::ErrorCode::invalid_state));
  CHECK(to_string(stills::ErrorCode::invalid_state) == "invalid_state");
}

// A source built from a fixed buffer can carry an embedded NUL; libavformat would have seen only
// the prefix and opened that instead, with no sign anything was dropped.
TEST_CASE("options: a source with an embedded NUL is rejected", "[open]") {
  const auto clip = fixture("counter.mp4").string();
  const std::string_view truncating{"\0/etc/passwd", 12};
  std::string with_nul = clip;
  with_nul.push_back('\0');
  with_nul += "and-more";
  REQUIRE_ERROR(AssetImageGenerator::open(std::string_view{with_nul}, sw_options()),
                ErrorCode::invalid_argument);
  REQUIRE_ERROR(AssetImageGenerator::open(truncating, sw_options()), ErrorCode::invalid_argument);
  REQUIRE_ERROR(AssetImageGenerator::open(std::string_view{}, sw_options()),
                ErrorCode::invalid_argument);
  CHECK(AssetImageGenerator::open(clip, sw_options()).has_value());  // the same path without one
}

// close() releases the asset where the scope cannot, and cancel_all() cancels a queued batch.
TEST_CASE("options: close() and cancel_all()", "[api]") {
  auto g = open_counter();
  Collector col;
  std::vector<Time> times;
  for (int n = 0; n < 40; ++n) times.emplace_back(n * 3, 30);
  auto req = g.generate_images(times, col.handler());
  g.cancel_all();
  REQUIRE(req.wait() == stills::WaitResult::finished);
  CHECK(col.count() == times.size());
  CHECK(col.count(stills::GenerationStatus::cancelled) > 0);

  CHECK(g.is_open());
  g.close();
  CHECK_FALSE(g.is_open());
  g.close();  // idempotent
  REQUIRE_ERROR(g.image_at(Time{1, 30}), ErrorCode::invalid_state);
}

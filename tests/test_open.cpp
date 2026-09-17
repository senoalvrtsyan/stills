#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::Time;

TEST_CASE("open: failure modes map to distinct error codes", "[open]") {
  SECTION("missing file") {
    auto g = AssetImageGenerator::open(fixture("does_not_exist.mp4").string(), sw_options());
    REQUIRE_ERROR(g, ErrorCode::file_not_found);
    REQUIRE(g.error().av_error == AVERROR(ENOENT));
    REQUIRE(to_string(g.error()).find("avformat_open_input") != std::string::npos);
  }
  SECTION("not a media file") {
    REQUIRE_ERROR(AssetImageGenerator::open(fixture("not_a_video.mp4").string(), sw_options()),
                  ErrorCode::unsupported_format);
  }
  SECTION("audio only") {
    REQUIRE_ERROR(AssetImageGenerator::open(fixture("audio_only.m4a").string(), sw_options()),
                  ErrorCode::no_video_stream);
  }
  SECTION("empty source") {
    REQUIRE_ERROR(AssetImageGenerator::open("", sw_options()), ErrorCode::invalid_argument);
  }
  SECTION("bad options") {
    stills::Options o = sw_options();
    o.maximum_size = stills::Size{-1, 0};
    REQUIRE_ERROR(AssetImageGenerator::open(fixture("counter.mp4").string(), o),
                  ErrorCode::invalid_argument);
    o = sw_options();
    o.tolerance.before = Time::invalid();
    REQUIRE_ERROR(AssetImageGenerator::open(fixture("counter.mp4").string(), o),
                  ErrorCode::invalid_argument);
    o = sw_options();
    o.video_stream_index = 7;
    REQUIRE_ERROR(AssetImageGenerator::open(fixture("counter.mp4").string(), o),
                  ErrorCode::invalid_argument);
  }
}

TEST_CASE("open: asset info for the primary fixture", "[open]") {
  auto g = open_counter();
  const auto& info = g.info();
  CHECK(info.container_name.find("mp4") != std::string::npos);
  CHECK(info.codec_name == "h264");
  CHECK(info.source_pixel_format == "yuv420p");
  CHECK(info.video_stream_index == 0);
  CHECK(info.time_base == stills::Rational{1, 15360});
  CHECK(info.average_frame_rate == stills::Rational{30, 1});
  REQUIRE(info.duration.has_value());
  CHECK(*info.duration == Time{4, 1});
  REQUIRE(info.frame_count.has_value());
  CHECK(*info.frame_count == 120);
  CHECK(info.coded_size == stills::Size{64, 48});
  CHECK(info.display_size == stills::Size{64, 48});
  CHECK(info.output_size == stills::Size{64, 48});
  CHECK(info.rotation_degrees == 0);
  REQUIRE(info.time_range().has_value());
  CHECK(info.time_range()->contains(Time{119, 30}));
  CHECK_FALSE(info.time_range()->contains(Time{4, 1}));
  CHECK_FALSE(g.active_decoder().hardware);
  CHECK(g.active_decoder().decoder_name == "h264");
  CHECK(g.options().hardware.policy == stills::HardwarePolicy::software_only);
}

TEST_CASE("open: MPEG-TS with a non-zero start time is asset-relative", "[open][sync]") {
  auto g =
      REQUIRE_OK(AssetImageGenerator::open(fixture("counter_offset.ts").string(), sw_options()));
  CHECK(g.info().container_name == "mpegts");
  CHECK(g.info().time_base == stills::Rational{1, 90000});
  REQUIRE(g.info().duration.has_value());
  CHECK(g.info().duration->to_seconds() > 3.9);
  for (int n : {0, 1, 30, 45, 119}) {
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    CHECK(frame_index_of(img) == n);
    CHECK(img.actual_time() == Time{n, 30});
  }
}

TEST_CASE("open: a single PNG is a one-frame asset", "[open][sync]") {
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("red.png").string(),
                                                sw_options(stills::PixelFormat::rgba)));
  CHECK(g.info().codec_name == "png");
  CHECK(g.info().coded_size == stills::Size{32, 32});
  auto img = REQUIRE_OK(g.image_at(Time::zero()));
  CHECK(img.size() == stills::Size{32, 32});
  const auto px = img.pixels();
  CHECK(std::to_integer<int>(px[0]) > 240);   // R
  CHECK(std::to_integer<int>(px[1]) < 16);    // G
  CHECK(std::to_integer<int>(px[2]) < 16);    // B
  CHECK(std::to_integer<int>(px[3]) == 255);  // A
  REQUIRE_ERROR(g.image_at(Time{1, 1}), ErrorCode::time_out_of_range);
}

TEST_CASE("open: anamorphic content is squared by default", "[open][sar]") {
  SECTION("applied") {
    auto g =
        REQUIRE_OK(AssetImageGenerator::open(fixture("counter_sar2.mp4").string(), sw_options()));
    CHECK(g.info().coded_size == stills::Size{64, 48});
    CHECK(g.info().display_size == stills::Size{128, 48});
    CHECK(g.info().output_size == stills::Size{128, 48});
    auto img = REQUIRE_OK(g.image_at(Time{41, 30}));
    CHECK(img.size() == stills::Size{128, 48});
    CHECK(frame_index_of(img) == 41);
  }
  SECTION("with a box") {
    stills::Options o = sw_options(stills::PixelFormat::rgba);
    o.maximum_size = stills::Size{64, 64};
    auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_sar2.mp4").string(), o));
    CHECK(g.info().output_size == stills::Size{64, 24});
    auto img = REQUIRE_OK(g.image_at(Time{41, 30}));
    CHECK(img.size() == stills::Size{64, 24});
    CHECK(frame_index_of(img) == 41);
  }
  SECTION("disabled") {
    stills::Options o = sw_options();
    o.apply_sample_aspect_ratio = false;
    auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_sar2.mp4").string(), o));
    CHECK(g.info().display_size == stills::Size{64, 48});
    auto img = REQUIRE_OK(g.image_at(Time{41, 30}));
    CHECK(img.size() == stills::Size{64, 48});
  }
}

TEST_CASE("open: 180 and 270 degree rotations", "[open][rotation]") {
  // ffmpeg's -display_rotation is counter-clockwise; rotation_degrees is the clockwise rotation
  // needed for upright display, so a 270° CCW display matrix reports as 90.
  struct Case {
    const char* file;
    int degrees;
    stills::Size size;
  };
  const Case c = GENERATE(Case{"counter_rot180.mp4", 180, stills::Size{64, 48}},
                          Case{"counter_rot270.mp4", 90, stills::Size{48, 64}});
  auto g = REQUIRE_OK(
      AssetImageGenerator::open(fixture(c.file).string(), sw_options(stills::PixelFormat::rgb24)));
  CHECK(g.info().rotation_degrees == c.degrees);
  CHECK(g.info().display_size == c.size);
  for (int n : {0, 11, 77}) {
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    CHECK(img.size() == c.size);
    CHECK(frame_index_of(img, c.degrees) == n);
  }
}

TEST_CASE("open: attached pictures are not video unless asked for", "[open]") {
  REQUIRE_ERROR(AssetImageGenerator::open(fixture("cover.m4a").string(), sw_options()),
                ErrorCode::no_video_stream);
  stills::Options o = sw_options(stills::PixelFormat::rgba);
  o.allow_attached_pictures = true;
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("cover.m4a").string(), o));
  CHECK(g.info().codec_name == "png");
  CHECK(g.info().coded_size == stills::Size{32, 32});
  auto img = REQUIRE_OK(g.image_at(Time::zero()));
  CHECK(std::to_integer<int>(img.pixels()[0]) > 240);  // red cover art
}

TEST_CASE("open: display-matrix rotation is applied by default", "[open][rotation]") {
  SECTION("applied") {
    auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_rot90.mp4").string(),
                                                  sw_options(stills::PixelFormat::rgba)));
    CHECK(g.info().rotation_degrees == 270);  // 90° counter-clockwise display matrix
    CHECK(g.info().coded_size == stills::Size{64, 48});
    CHECK(g.info().display_size == stills::Size{48, 64});
    CHECK(g.info().output_size == stills::Size{48, 64});
    for (int n : {0, 7, 31, 100}) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      CHECK(img.size() == stills::Size{48, 64});
      CHECK(frame_index_of(img, g.info().rotation_degrees) == n);
    }
  }
  SECTION("not applied when disabled") {
    stills::Options o = sw_options(stills::PixelFormat::yuv420p);
    o.apply_preferred_track_transform = false;
    auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_rot90.mp4").string(), o));
    CHECK(g.info().display_size == stills::Size{64, 48});
    auto img = REQUIRE_OK(g.image_at(Time{31, 30}));
    CHECK(img.size() == stills::Size{64, 48});
    CHECK(frame_index_of(img) == 31);
  }
  SECTION("rotation with a fitted box applies to the rotated output") {
    stills::Options o = sw_options(stills::PixelFormat::rgb24);
    o.maximum_size = stills::Size{24, 24};
    auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_rot90.mp4").string(), o));
    CHECK(g.info().output_size == stills::Size{18, 24});
    auto img = REQUIRE_OK(g.image_at(Time{5, 30}));
    CHECK(img.size() == stills::Size{18, 24});
    CHECK(frame_index_of(img, g.info().rotation_degrees) == 5);
  }
}

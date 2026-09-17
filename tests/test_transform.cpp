// Display-matrix mirroring, the transform kernel on an asymmetric pattern, RGB(A) sources and
// odd output dimensions.
#include <array>
#include <cstring>
#include <stills/detail/pipeline.hpp>
#include <stills/detail/transform.hpp>
#include <stills/interop.hpp>

#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::PixelFormat;
using stills::Size;
using stills::Time;

TEST_CASE("transform: display matrices with a reflection are decoded as rotate-then-mirror",
          "[rotation][unit]") {
  using stills::detail::decode_display_matrix;
  const auto fixed = [](double v) { return static_cast<std::int32_t>(v * 65536.0); };
  // Built with libav's own helpers so the convention is exactly the one the demuxers produce.
  const auto matrix = [&](double ccw_degrees, bool hflip) {
    std::array<std::int32_t, 9> m{};
    av_display_rotation_set(m.data(), ccw_degrees);
    if (hflip) av_display_matrix_flip(m.data(), 1, 0);
    return m;
  };
  const auto t0 = decode_display_matrix(matrix(0, false).data());
  CHECK(t0.rotation == 0);
  CHECK_FALSE(t0.mirrored);
  const auto hflip = decode_display_matrix(matrix(0, true).data());
  CHECK(hflip.rotation == 0);
  CHECK(hflip.mirrored);
  std::array<std::int32_t, 9> vflip{fixed(1), 0, 0, 0, fixed(-1), 0, 0, 0, fixed(1)};
  const auto v = decode_display_matrix(vflip.data());
  CHECK(v.rotation == 180);  // vertical flip == rotate 180 then mirror horizontally
  CHECK(v.mirrored);
  // av_display_rotation_set(90) decodes as a 90 degree clockwise display rotation here (ffmpeg's
  // -display_rotation option negates before calling it; the fixture tests cover that convention).
  const auto r90 = decode_display_matrix(matrix(90, false).data());
  CHECK(r90.rotation == 90);
  CHECK_FALSE(r90.mirrored);
  const auto r180 = decode_display_matrix(matrix(180, false).data());
  const auto r270 = decode_display_matrix(matrix(270, false).data());
  CHECK(r270.rotation == 270);
  CHECK_FALSE(r270.mirrored);
  const auto r90m = decode_display_matrix(matrix(90, true).data());
  CHECK(r90m.rotation == 90);  // the mirror is factored out; the rotation is unchanged
  CHECK(r90m.mirrored);
  CHECK(r180.rotation == 180);
  CHECK_FALSE(r180.mirrored);
  // A transposition (reflection across the diagonal) is 90 + mirror or 270 + mirror.
  std::array<std::int32_t, 9> transpose{0, fixed(1), 0, fixed(1), 0, 0, 0, 0, fixed(1)};
  const auto tr = decode_display_matrix(transpose.data());
  CHECK(tr.mirrored);
  CHECK((tr.rotation == 90 || tr.rotation == 270));
}

TEST_CASE("transform: kernel maps every pixel of an asymmetric pattern correctly",
          "[rotation][unit]") {
  // 3x2 RGBA source: pixel (x, y) holds {x, y, 0, 255}.
  const int W = 3, H = 2;
  auto make_src = [&] {
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_RGBA;
    f->width = W;
    f->height = H;
    REQUIRE(av_frame_get_buffer(f, 0) == 0);
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        std::uint8_t* p = f->data[0] + y * f->linesize[0] + x * 4;
        p[0] = static_cast<std::uint8_t>(x);
        p[1] = static_cast<std::uint8_t>(y);
        p[2] = 0;
        p[3] = 255;
      }
    }
    return f;
  };
  AVFrame* src = make_src();
  struct Case {
    int degrees;
    bool mirror;
  };
  const Case c = GENERATE(Case{0, false}, Case{90, false}, Case{180, false}, Case{270, false},
                          Case{0, true}, Case{90, true}, Case{180, true}, Case{270, true});
  INFO("rotate " << c.degrees << (c.mirror ? " + mirror" : ""));
  auto out =
      REQUIRE_OK(stills::detail::transform_frame(*src, PixelFormat::rgba, c.degrees, c.mirror));
  const bool swap = c.degrees == 90 || c.degrees == 270;
  const int OW = swap ? H : W;
  const int OH = swap ? W : H;
  REQUIRE(out->width == OW);
  REQUIRE(out->height == OH);
  // Expected: where does source (x, y) land? rotate clockwise, then mirror horizontally.
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      int dx = x, dy = y;
      switch (c.degrees) {
        case 90:
          dx = H - 1 - y;
          dy = x;
          break;
        case 180:
          dx = W - 1 - x;
          dy = H - 1 - y;
          break;
        case 270:
          dx = y;
          dy = W - 1 - x;
          break;
        default:
          break;
      }
      if (c.mirror) dx = OW - 1 - dx;
      const std::uint8_t* p = out->data[0] + dy * out->linesize[0] + dx * 4;
      CHECK(int{p[0]} == x);
      CHECK(int{p[1]} == y);
      CHECK(int{p[3]} == 255);
    }
  }
  av_frame_free(&src);
}

TEST_CASE("transform: mirrored display matrices are applied, not read as rotations",
          "[open][rotation]") {
  SECTION("horizontal flip") {
    auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_hflip.mp4").string(),
                                                  sw_options(PixelFormat::rgba)));
    CHECK(g.info().rotation_degrees == 0);
    CHECK(g.info().mirrored);
    CHECK(g.info().display_size == Size{64, 48});
    for (int n : {7, 45, 100}) {  // asymmetric indices (100 -> nibbles 4/6), so a mirror is visible
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      // The luma index lives in the left/right halves; after a mirror they are swapped, which is
      // what the "180" reading position sees (rows are uniform).
      CHECK(frame_index_of(img, 180) == n);
      CHECK(frame_index_of(img, 0) != n);
    }
  }
  SECTION("vertical flip == rotate 180 + mirror: columns end up where they started") {
    auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_vflip.mp4").string(),
                                                  sw_options(PixelFormat::rgba)));
    CHECK(g.info().rotation_degrees == 180);
    CHECK(g.info().mirrored);
    for (int n : {7, 45}) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      CHECK(frame_index_of(img, 0) == n);
    }
  }
  SECTION("transform disabled: coded orientation, matrix reported, side data kept") {
    stills::Options o = sw_options(PixelFormat::rgba);
    o.apply_preferred_track_transform = false;
    auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_hflip.mp4").string(), o));
    CHECK(g.info().mirrored);
    auto img = REQUIRE_OK(g.image_at(Time{7, 30}));
    CHECK(frame_index_of(img, 0) == 7);
    const AVFrame* f = stills::interop::native_frame(img);
    CHECK(av_frame_get_side_data(f, AV_FRAME_DATA_DISPLAYMATRIX) != nullptr);
  }
}

TEST_CASE("transform: output frames carry no display matrix once it has been applied",
          "[rotation][interop]") {
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_rot90.mp4").string(),
                                                sw_options(PixelFormat::rgba)));
  auto img = REQUIRE_OK(g.image_at(Time{7, 30}));
  const AVFrame* f = stills::interop::native_frame(img);
  REQUIRE(f != nullptr);
  CHECK(av_frame_get_side_data(f, AV_FRAME_DATA_DISPLAYMATRIX) == nullptr);
  CHECK(img.size() == Size{48, 64});
  CHECK(img.sample_aspect_ratio() == stills::Rational{1, 1});
}

TEST_CASE("transform: SAR is preserved on the output frame when not applied", "[sar][interop]") {
  stills::Options o = sw_options(PixelFormat::rgba);
  o.apply_sample_aspect_ratio = false;
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_sar2.mp4").string(), o));
  CHECK(g.info().sample_aspect_ratio == stills::Rational{2, 1});
  auto img = REQUIRE_OK(g.image_at(Time{4, 30}));
  CHECK(img.size() == Size{64, 48});
  CHECK(img.sample_aspect_ratio() == stills::Rational{2, 1});
  auto squared = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_sar2.mp4").string(),
                                                      sw_options(PixelFormat::rgba)));
  auto sq = REQUIRE_OK(squared.image_at(Time{4, 30}));
  CHECK(sq.sample_aspect_ratio() == stills::Rational{1, 1});
}

TEST_CASE("transform: RGBA source colour range is labelled as converted", "[output][color]") {
  // alpha.mov: PNG frames, blue at 50 % alpha, full-range RGB.
  SECTION("to yuv420p: limited range, Y of pure blue about 41") {
    auto g = REQUIRE_OK(
        AssetImageGenerator::open(fixture("alpha.mov").string(), sw_options(PixelFormat::yuv420p)));
    CHECK(g.info().source_pixel_format == "rgba");
    auto img = REQUIRE_OK(g.image_at(Time::zero()));
    CHECK(img.color_range() == stills::ColorRange::limited);
    const int y = std::to_integer<int>(img.plane(0)[24 * img.row_stride(0) + 32]);
    CHECK(std::abs(y - 41) <= 3);  // BT.601 limited-range Y of (0,0,255)
  }
  SECTION("to nv12: same") {
    auto g = REQUIRE_OK(
        AssetImageGenerator::open(fixture("alpha.mov").string(), sw_options(PixelFormat::nv12)));
    auto img = REQUIRE_OK(g.image_at(Time::zero()));
    CHECK(img.color_range() == stills::ColorRange::limited);
    const int y = std::to_integer<int>(img.plane(0)[24 * img.row_stride(0) + 32]);
    CHECK(std::abs(y - 41) <= 3);
  }
  SECTION("to rgba: full range, alpha survives") {
    auto g = REQUIRE_OK(
        AssetImageGenerator::open(fixture("alpha.mov").string(), sw_options(PixelFormat::rgba)));
    auto img = REQUIRE_OK(g.image_at(Time::zero()));
    CHECK(img.color_range() == stills::ColorRange::full);
    const auto px = img.pixels();
    const std::size_t at = 24 * img.row_stride(0) + 32 * 4;
    CHECK(std::to_integer<int>(px[at + 2]) > 240);                 // B
    CHECK(std::to_integer<int>(px[at + 0]) < 16);                  // R
    CHECK(std::abs(std::to_integer<int>(px[at + 3]) - 128) <= 2);  // A
  }
  SECTION("to gray8: full range") {
    auto g = REQUIRE_OK(
        AssetImageGenerator::open(fixture("alpha.mov").string(), sw_options(PixelFormat::gray8)));
    auto img = REQUIRE_OK(g.image_at(Time::zero()));
    CHECK(img.color_range() == stills::ColorRange::full);
    const int y = std::to_integer<int>(img.pixels()[24 * img.row_stride(0) + 32]);
    CHECK(std::abs(y - 29) <= 3);  // full-range Y of pure blue
  }
}

TEST_CASE("transform: odd source dimensions per output format", "[output]") {
  struct Case {
    PixelFormat fmt;
    Size expected;
  };
  const Case c =
      GENERATE(Case{PixelFormat::rgb24, Size{33, 17}}, Case{PixelFormat::rgba, Size{33, 17}},
               Case{PixelFormat::gray8, Size{33, 17}}, Case{PixelFormat::yuv420p, Size{32, 16}},
               Case{PixelFormat::nv12, Size{32, 16}});
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("odd.mov").string(), sw_options(c.fmt)));
  CHECK(g.info().coded_size == Size{33, 17});
  CHECK(g.info().output_size == c.expected);
  auto img = REQUIRE_OK(g.image_at(Time::zero()));
  CHECK(img.size() == c.expected);
  CHECK(img.row(0, 0).size() == img.row_stride(0));
  CHECK(img.row(0, c.expected.height).empty());
}

#include "support/stills_TestCommon.h"

using namespace testsupport;
using stills::PixelFormat;
using stills::Size;
using stills::Time;

TEST_CASE ("output: maximumSize fits within the box, preserves aspect, never upscales", "[output]")
{
    struct Case
    {
        std::optional<Size> max;
        Size expected;
        PixelFormat fmt;
    };

    const Case c = GENERATE (Case{ std::nullopt, Size{ 64, 48 }, PixelFormat::rgba },
                             Case{ Size{ 32, 0 }, Size{ 32, 24 }, PixelFormat::rgba },
                             Case{ Size{ 0, 12 }, Size{ 16, 12 }, PixelFormat::rgba },
                             Case{ Size{ 20, 20 }, Size{ 20, 15 }, PixelFormat::rgba },
                             Case{ Size{ 1000, 1000 }, Size{ 64, 48 }, PixelFormat::rgba },
                             Case{ Size{ 21, 21 }, Size{ 20, 16 }, PixelFormat::yuv420p }, // 21x16 rounded down to even
                             Case{ Size{ 21, 21 }, Size{ 20, 16 }, PixelFormat::nv12 },
                             Case{ Size{ 21, 21 }, Size{ 21, 16 }, PixelFormat::rgb24 });
    stills::Options o = swOptions (c.fmt);
    o.maximumSize = c.max;
    auto g = openCounter (o);
    CHECK (g.getInfo().outputSize == c.expected);
    auto img = REQUIRE_OK (g.imageAt (Time{ 33, 30 }));
    CHECK (img.getSize() == c.expected);
    CHECK (frameIndexOf (img) == 33);
}

TEST_CASE ("output: every pixel format has the documented plane layout", "[output]")
{
    const PixelFormat fmt = GENERATE (PixelFormat::rgba, PixelFormat::bgra, PixelFormat::argb, PixelFormat::abgr,
                                      PixelFormat::rgb0, PixelFormat::bgr0, PixelFormat::rgb24, PixelFormat::bgr24,
                                      PixelFormat::gray8, PixelFormat::nv12, PixelFormat::yuv420p);
    INFO ("format " << fmt);
    auto g = openCounter (swOptions (fmt));
    auto img = REQUIRE_OK (g.imageAt (Time{ 60, 30 }));
    CHECK (img.getPixelFormat() == fmt);
    CHECK (img.getPlaneCount() == stills::getPlaneCount (fmt));

    for (int p = 0; p < img.getPlaneCount(); ++p)
    {
        const Size ps = img.getPlaneSize (p);
        const auto minimal =
            static_cast<std::size_t> (ps.width) * static_cast<std::size_t> (stills::getBytesPerPixel (fmt, p));
        CHECK (img.getRowStride (p) >= minimal);
        CHECK (img.getPlane (p).size() == img.getRowStride (p) * static_cast<std::size_t> (ps.height));
    }

    if (stills::hasChromaSubsampling (fmt))
    {
        CHECK (img.getPlaneSize (1) == Size{ 32, 24 });
        CHECK (img.getColorRange() == stills::ColorRange::limited);
    }
    else
    {
        CHECK (img.getColorRange() == stills::ColorRange::full);
    }

    CHECK (frameIndexOf (img) == 60);
    // Packed copy has no padding and the same content.
    const auto packed = REQUIRE_OK (img.toPackedBytes());
    CHECK (packed.size() == img.getPackedSizeBytes());
    const auto expectBytes =
        static_cast<std::size_t> (64 * 48)
        * (stills::hasChromaSubsampling (fmt) ? 3 : 2 * static_cast<std::size_t> (stills::getBytesPerPixel (fmt))) / 2;
    CHECK (packed.size() == expectBytes);
}

TEST_CASE ("output: every scaler produces the right frame at the right size", "[output]")
{
    const stills::Scaler scaler = GENERATE (stills::Scaler::fastBilinear, stills::Scaler::bilinear,
                                            stills::Scaler::bicubic, stills::Scaler::area, stills::Scaler::lanczos);
    stills::Options o = swOptions (PixelFormat::rgba);
    o.scaler = scaler;
    o.maximumSize = Size{ 40, 0 };
    auto g = openCounter (o);
    auto img = REQUIRE_OK (g.imageAt (Time{ 66, 30 }));
    CHECK (img.getSize() == Size{ 40, 30 });
    CHECK (frameIndexOf (img) == 66);
}

TEST_CASE ("output: decoder thread count does not affect results", "[output]")
{
    const int threads = GENERATE (1, 2, 8);
    stills::Options o = swOptions();
    o.decoderThreads = threads;
    auto g = openCounter (o);

    for (int n : { 0, 29, 30, 31, 119, 45 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        CHECK (frameIndexOf (img) == n);
    }
}

TEST_CASE ("output: channel order and values", "[output]")
{
    auto g = openCounter (swOptions (PixelFormat::rgba));
    auto img = REQUIRE_OK (g.imageAt (Time{ 60, 30 }));
    // Frame 60: left half Y = 22 + 14*12 = 190, right half Y = 22 + 14*3 = 64 (limited range).
    const auto px = img.getPixels();
    const auto at = [&] (int x, int y, int c)
    {
        return std::to_integer<int> (px[static_cast<std::size_t> (y) * img.getRowStride (0)
                                        + static_cast<std::size_t> (x) * 4 + static_cast<std::size_t> (c)]);
    };

    const int left = static_cast<int> (std::lround ((190 - 16) * 255.0 / 219.0));
    const int right = static_cast<int> (std::lround ((64 - 16) * 255.0 / 219.0));

    for (int c = 0; c < 3; ++c)
    {
        CHECK (std::abs (at (16, 24, c) - left) <= 2);
        CHECK (std::abs (at (48, 24, c) - right) <= 2);
    }

    CHECK (at (16, 24, 3) == 255); // alpha
    auto g2 = openCounter (swOptions (PixelFormat::gray8));
    auto gray = REQUIRE_OK (g2.imageAt (Time{ 60, 30 }));
    CHECK (std::abs (std::to_integer<int> (gray.getPixels()[24 * gray.getRowStride (0) + 16]) - left) <= 2);
}

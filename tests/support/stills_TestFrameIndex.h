#pragma once
// Recovers the frame index encoded in the fixtures' luma (see tests/fixtures/generate.sh) from an
// Image in any of the supported output formats.
#include <cmath>
#include <cstddef>
#include <optional>
#include <stills/stills_Image.h>

namespace testsupport
{

/// Limited-range luma of the pixel at (x, y) as a double (RGB is converted back to Y').
inline double lumaAt (const stills::Image& img, int x, int y)
{
    using stills::PixelFormat;
    const auto p0 = img.getPlane (0);
    const std::size_t stride = img.getRowStride (0);
    const auto at = [&] (int planeXBytes)
    {
        return std::to_integer<int> (
            p0[static_cast<std::size_t> (y) * stride + static_cast<std::size_t> (planeXBytes)]);
    };

    switch (img.getPixelFormat())
    {
    case PixelFormat::gray8:
        return at (x) * 219.0 / 255.0 + 16.0; // gray output is full range
    case PixelFormat::yuv420p:
    case PixelFormat::nv12:
        return at (x); // Y plane, limited range preserved
    case PixelFormat::rgb24:
    case PixelFormat::bgr24:
        return at (x * 3 + 1) * 219.0 / 255.0 + 16.0; // G channel
    case PixelFormat::rgba:
    case PixelFormat::bgra:
    case PixelFormat::rgb0:
    case PixelFormat::bgr0:
        return at (x * 4 + 1) * 219.0 / 255.0 + 16.0;
    case PixelFormat::argb:
    case PixelFormat::abgr:
        return at (x * 4 + 2) * 219.0 / 255.0 + 16.0;
    case PixelFormat::p010:
    { // 10 bits in the top of a 16-bit little-endian sample
        const int v = at (x * 2) | (at (x * 2 + 1) << 8);
        return (v >> 6) / 4.0; // back to 8-bit limited-range luma
    }

    case PixelFormat::rgba64:
    { // 16-bit G channel
        const int g = at (x * 8 + 2) | (at (x * 8 + 3) << 8);
        return (g / 257.0) * 219.0 / 255.0 + 16.0;
    }
    }

    return 0.0;
}

/// Frame index encoded in the image. `rotation` = clockwise degrees the fixture was rotated by.
inline int frameIndexOf (const stills::Image& img, int rotation = 0)
{
    const int w = img.getWidth();
    const int h = img.getHeight();
    double lo = 0;
    double hi = 0;
    switch (rotation)
    {
    case 0:
        lo = lumaAt (img, w / 4, h / 2);
        hi = lumaAt (img, 3 * w / 4, h / 2);
        break;
    case 90:
        lo = lumaAt (img, w / 2, h / 4);
        hi = lumaAt (img, w / 2, 3 * h / 4);
        break; // left -> top
    case 180:
        lo = lumaAt (img, 3 * w / 4, h / 2);
        hi = lumaAt (img, w / 4, h / 2);
        break;
    case 270:
        lo = lumaAt (img, w / 2, 3 * h / 4);
        hi = lumaAt (img, w / 2, h / 4);
        break; // left -> bottom
    default:
        break;
    }

    const int klo = static_cast<int> (std::lround ((lo - 22.0) / 14.0));
    const int khi = static_cast<int> (std::lround ((hi - 22.0) / 14.0));
    return klo + 16 * khi;
}

/// The frame index a fixture time corresponds to (30 fps).
inline int frameIndexOfTime (stills::Time t)
{
    return static_cast<int> (t.toTimestamp (stills::Rational{ 1, 30 }, stills::TimeRounding::down));
}

} // namespace testsupport

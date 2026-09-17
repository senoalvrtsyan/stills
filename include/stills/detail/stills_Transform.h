#pragma once
// stills/detail/stills_Transform.h — 90/180/270 degree rotation and horizontal mirroring of a software
// frame, plane by plane. The eight orientations of the dihedral group are expressed as "rotate
// clockwise by `degrees`, then mirror horizontally if `mirror`".

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

#include "stills/detail/stills_FFmpeg.h"
#include "stills/stills_PixelFormat.h"

namespace stills::detail
{

/// Per-row mapping of a source row onto the destination: the destination cell of source (0, y) is
/// (x0, y0) and every step in source x moves the destination by (dx, dy) cells.
struct RowMap
{
    std::ptrdiff_t x0{ 0 }, y0{ 0 }, dx{ 1 }, dy{ 0 };
};

/// Where source row `y` of a `w` x `h` plane lands after rotating clockwise by `degrees` (the
/// destination is `h` x `w` for 90/270) and optionally mirroring the result horizontally.
[[nodiscard]] constexpr RowMap rowMap (int degrees, bool mirror, int w, int h, int y) noexcept
{
    RowMap m;
    int dstW = w;
    switch (degrees)
    {
    case 90: // (x, y) -> (h-1-y, x)
        m = RowMap{ h - 1 - y, 0, 0, 1 };
        dstW = h;
        break;
    case 180: // (x, y) -> (w-1-x, h-1-y)
        m = RowMap{ w - 1, h - 1 - y, -1, 0 };
        break;
    case 270: // (x, y) -> (y, w-1-x)
        m = RowMap{ y, w - 1, 0, -1 };
        dstW = h;
        break;
    default:
        m = RowMap{ 0, y, 1, 0 };
        break;
    }

    if (mirror)
    {
        m.x0 = dstW - 1 - m.x0;
        m.dx = -m.dx;
    }

    return m;
}

/// Copies one plane through `rowMap`. `Bpp` is the cell size in bytes; the loop body is a fixed-
/// size copy so the compiler vectorises the common (contiguous) cases.
template <int Bpp>
inline void transformPlaneBpp (const std::uint8_t* src, int srcStride, int w, int h, std::uint8_t* dst, int dstStride,
                               int degrees, bool mirror) noexcept
{
    const auto ss = static_cast<std::ptrdiff_t> (srcStride);
    const auto ds = static_cast<std::ptrdiff_t> (dstStride);

    for (int y = 0; y < h; ++y)
    {
        const std::uint8_t* row = src + static_cast<std::ptrdiff_t> (y) * ss;
        const RowMap m = rowMap (degrees, mirror, w, h, y);
        std::uint8_t* out = dst + m.y0 * ds + m.x0 * Bpp;
        const std::ptrdiff_t step = m.dy * ds + m.dx * Bpp;

        if (step == Bpp)
        { // straight copy (identity, or the untouched middle of a mirror-free row)
            std::memcpy (out, row, static_cast<std::size_t> (w) * Bpp);
            continue;
        }

        for (int x = 0; x < w; ++x, out += step, row += Bpp)
        {
            std::memcpy (out, row, static_cast<std::size_t> (Bpp));
        }
    }
}

/// Dispatches on the cell size (1 = gray / planar Y,U,V; 2 = NV12 chroma pairs / P010 luma; 3 =
/// RGB; 4 = RGBA / P010 chroma pairs; 8 = RGBA64).
inline void transformPlane (const std::uint8_t* src, int srcStride, int w, int h, int bpp, std::uint8_t* dst,
                            int dstStride, int degrees, bool mirror) noexcept
{
    switch (bpp)
    {
    case 1:
        transformPlaneBpp<1> (src, srcStride, w, h, dst, dstStride, degrees, mirror);
        break;
    case 2:
        transformPlaneBpp<2> (src, srcStride, w, h, dst, dstStride, degrees, mirror);
        break;
    case 3:
        transformPlaneBpp<3> (src, srcStride, w, h, dst, dstStride, degrees, mirror);
        break;
    case 4:
        transformPlaneBpp<4> (src, srcStride, w, h, dst, dstStride, degrees, mirror);
        break;
    case 8:
        transformPlaneBpp<8> (src, srcStride, w, h, dst, dstStride, degrees, mirror);
        break;
    default:
        break; // unreachable: every software PixelFormat has 1, 2, 3, 4 or 8 byte cells
    }
}

/// Writes `src` rotated clockwise by `degrees` (then mirrored horizontally when `mirror` is set)
/// into `dst`, an allocated frame of the transposed size (for 90/270) in the same format. `src`
/// must be one of our output PixelFormats (software).
[[nodiscard]] inline std::expected<void, Error> transformInto (const AVFrame& src, AVFrame& dst, PixelFormat fmt,
                                                               int degrees, bool mirror)
{
    const bool swap = degrees == 90 || degrees == 270;

    if (dst.format != src.format || dst.width != (swap ? src.height : src.width)
        || dst.height != (swap ? src.width : src.height))
    {
        return fail (ErrorCode::internal, "transformInto: destination geometry does not match");
    }

    if (int r = av_frame_copy_props (&dst, &src); r < 0)
    {
        return fail (ErrorCode::conversionFailed, r, "av_frame_copy_props(transformed)");
    }

    const int planes = getPlaneCount (fmt);
    const int shift = getChromaShift (fmt);

    for (int p = 0; p < planes; ++p)
    {
        const int sh = p == 0 ? 0 : shift;
        const int w = (src.width + (1 << sh) - 1) >> sh;
        const int h = (src.height + (1 << sh) - 1) >> sh;
        transformPlane (src.data[p], src.linesize[p], w, h, getBytesPerPixel (fmt, p), dst.data[p], dst.linesize[p],
                        degrees, mirror);
    }

    return {};
}

/// Returns a new frame containing `src` rotated clockwise by `degrees` and then mirrored
/// horizontally when `mirror` is set. `src` must be one of our output PixelFormats (software,
/// 8-bit).
[[nodiscard]] inline std::expected<FramePtr, Error> transformFrame (const AVFrame& src, PixelFormat fmt, int degrees,
                                                                    bool mirror)
{
    auto dst = makeFrame();

    if (! dst) return std::unexpected (dst.error());
    const bool swap = degrees == 90 || degrees == 270;
    (*dst)->format = src.format;
    (*dst)->width = swap ? src.height : src.width;
    (*dst)->height = swap ? src.width : src.height;

    if (int r = av_frame_get_buffer (dst->get(), 0); r < 0)
    {
        return fail (ErrorCode::outOfMemory, r, "av_frame_get_buffer(transformed)");
    }

    if (auto r = transformInto (src, **dst, fmt, degrees, mirror); ! r) return std::unexpected (r.error());
    return dst;
}

} // namespace stills::detail

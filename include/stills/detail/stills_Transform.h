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

// Per-row mapping of a source row onto the destination: the destination cell of source (0, y) is
// (x0, y0) and every step in source x moves the destination by (dx, dy) cells.
struct RowMap
{
    std::ptrdiff_t x0{ 0 }, y0{ 0 }, dx{ 1 }, dy{ 0 };
};

// Where source row `y` of a `w` x `h` plane lands after rotating clockwise by `degrees` (the
// destination is `h` x `w` for 90/270) and optionally mirroring the result horizontally.
[[nodiscard]] constexpr RowMap computeRowMap (int degrees, bool mirror, int width, int height, int row) noexcept
{
    RowMap map;
    int destinationWidth = width;
    switch (degrees)
    {
    case 90: // (x, y) -> (h-1-y, x)
        map = RowMap{ height - 1 - row, 0, 0, 1 };
        destinationWidth = height;
        break;
    case 180: // (x, y) -> (w-1-x, h-1-y)
        map = RowMap{ width - 1, height - 1 - row, -1, 0 };
        break;
    case 270: // (x, y) -> (y, w-1-x)
        map = RowMap{ row, width - 1, 0, -1 };
        destinationWidth = height;
        break;
    default:
        map = RowMap{ 0, row, 1, 0 };
        break;
    }

    if (mirror)
    {
        map.x0 = destinationWidth - 1 - map.x0;
        map.dx = -map.dx;
    }

    return map;
}

// Copies one plane through `computeRowMap`. `Bpp` is the cell size in bytes; the loop body is a fixed-
// size copy so the compiler vectorises the common (contiguous) cases.
template <int BytesPerCell>
inline void transformPlaneBpp (const std::uint8_t* source, int srcStride, int width, int height,
                               std::uint8_t* destination, int dstStride, int degrees, bool mirror) noexcept
{
    const auto sourceStride = static_cast<std::ptrdiff_t> (srcStride);
    const auto destinationStride = static_cast<std::ptrdiff_t> (dstStride);

    for (int row = 0; row < height; ++row)
    {
        const std::uint8_t* sourceRow = source + static_cast<std::ptrdiff_t> (row) * sourceStride;
        const RowMap map = computeRowMap (degrees, mirror, width, height, row);
        std::uint8_t* cell = destination + map.y0 * destinationStride + map.x0 * BytesPerCell;
        const std::ptrdiff_t step = map.dy * destinationStride + map.dx * BytesPerCell;

        if (step == BytesPerCell)
        { // straight copy (identity, or the untouched middle of a mirror-free row)
            std::memcpy (cell, sourceRow, static_cast<std::size_t> (width) * BytesPerCell);
            continue;
        }

        for (int column = 0; column < width; ++column, cell += step, sourceRow += BytesPerCell)
        {
            std::memcpy (cell, sourceRow, static_cast<std::size_t> (BytesPerCell));
        }
    }
}

// Dispatches on the cell size (1 = gray / planar Y,U,V; 2 = NV12 chroma pairs / P010 luma; 3 =
// RGB; 4 = RGBA / P010 chroma pairs; 8 = RGBA64).
inline void transformPlane (const std::uint8_t* source, int srcStride, int width, int height, int bytesPerCell,
                            std::uint8_t* destination, int dstStride, int degrees, bool mirror) noexcept
{
    switch (bytesPerCell)
    {
    case 1:
        transformPlaneBpp<1> (source, srcStride, width, height, destination, dstStride, degrees, mirror);
        break;
    case 2:
        transformPlaneBpp<2> (source, srcStride, width, height, destination, dstStride, degrees, mirror);
        break;
    case 3:
        transformPlaneBpp<3> (source, srcStride, width, height, destination, dstStride, degrees, mirror);
        break;
    case 4:
        transformPlaneBpp<4> (source, srcStride, width, height, destination, dstStride, degrees, mirror);
        break;
    case 8:
        transformPlaneBpp<8> (source, srcStride, width, height, destination, dstStride, degrees, mirror);
        break;
    default:
        break; // unreachable: every software PixelFormat has 1, 2, 3, 4 or 8 byte cells
    }
}

// Writes `src` rotated clockwise by `degrees` (then mirrored horizontally when `mirror` is set)
// into `dst`, an allocated frame of the transposed size (for 90/270) in the same format. `src`
// must be one of our output PixelFormats (software).
[[nodiscard]] inline std::expected<void, Error> transformInto (const AVFrame& source, AVFrame& destination,
                                                               PixelFormat pixelFormat, int degrees, bool mirror)
{
    const bool swap = degrees == 90 || degrees == 270;

    if (destination.format != source.format || destination.width != (swap ? source.height : source.width)
        || destination.height != (swap ? source.width : source.height))
    {
        return fail (ErrorCode::internal, "transformInto: destination geometry does not match");
    }

    if (int result = av_frame_copy_props (&destination, &source); result < 0)
    {
        return fail (ErrorCode::conversionFailed, result, "av_frame_copy_props(transformed)");
    }

    const int planes = getPlaneCount (pixelFormat);
    const int shift = getChromaShift (pixelFormat);

    for (int plane = 0; plane < planes; ++plane)
    {
        const int planeShift = plane == 0 ? 0 : shift;
        const int width = (source.width + (1 << planeShift) - 1) >> planeShift;
        const int height = (source.height + (1 << planeShift) - 1) >> planeShift;
        transformPlane (source.data[plane], source.linesize[plane], width, height,
                        getBytesPerPixel (pixelFormat, plane), destination.data[plane], destination.linesize[plane],
                        degrees, mirror);
    }

    return {};
}

// Returns a new frame containing `src` rotated clockwise by `degrees` and then mirrored
// horizontally when `mirror` is set. `src` must be one of our output PixelFormats (software,
// 8-bit).
[[nodiscard]] inline std::expected<FramePtr, Error> transformFrame (const AVFrame& source, PixelFormat pixelFormat,
                                                                    int degrees, bool mirror)
{
    auto destination = makeFrame();

    if (! destination) return std::unexpected (destination.error());
    const bool swap = degrees == 90 || degrees == 270;
    (*destination)->format = source.format;
    (*destination)->width = swap ? source.height : source.width;
    (*destination)->height = swap ? source.width : source.height;

    if (int result = av_frame_get_buffer (destination->get(), 0); result < 0)
    {
        return fail (ErrorCode::outOfMemory, result, "av_frame_get_buffer(transformed)");
    }

    if (auto result = transformInto (source, **destination, pixelFormat, degrees, mirror); ! result)
        return std::unexpected (result.error());
    return destination;
}

} // namespace stills::detail

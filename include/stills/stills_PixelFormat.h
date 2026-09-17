#pragma once
// stills/stills_PixelFormat.h — output pixel formats the library can produce. No FFmpeg dependency;
// the mapping to AVPixelFormat lives in <stills/stills_Interop.h>.

#include <cstdint>
#include <ostream>
#include <string_view>
#include <version>

#include "stills/detail/stills_Config.h"

namespace stills
{

/// Pixel layouts a generated Image can be requested in. `rgba` is the default.
enum class PixelFormat : std::uint8_t
{
    rgba,    ///< 4 bytes/pixel, one plane: R G B A
    bgra,    ///< 4 bytes/pixel, one plane: B G R A
    argb,    ///< 4 bytes/pixel, one plane: A R G B
    abgr,    ///< 4 bytes/pixel, one plane: A B G R
    rgb0,    ///< 4 bytes/pixel, one plane: R G B x (padding byte undefined)
    bgr0,    ///< 4 bytes/pixel, one plane: B G R x (padding byte undefined)
    rgb24,   ///< 3 bytes/pixel, one plane
    bgr24,   ///< 3 bytes/pixel, one plane
    gray8,   ///< 1 byte/pixel luma, one plane, full range
    nv12,    ///< planar Y + interleaved UV, 4:2:0 (two planes)
    yuv420p, ///< planar Y, U, V, 4:2:0 (three planes)
    p010,    ///< planar Y + interleaved UV, 4:2:0, 10 bits in 16-bit little-endian samples (two planes)
    rgba64,  ///< 8 bytes/pixel, one plane: R G B A as 16-bit little-endian samples
};

/// Nominal range of YUV samples. RGB and gray outputs are always full range.
enum class ColorRange : std::uint8_t
{
    unspecified,
    limited,
    full
};

[[nodiscard]] constexpr std::string_view toString (PixelFormat f) noexcept
{
    switch (f)
    {
    case PixelFormat::rgba:
        return "rgba";
    case PixelFormat::bgra:
        return "bgra";
    case PixelFormat::argb:
        return "argb";
    case PixelFormat::abgr:
        return "abgr";
    case PixelFormat::rgb0:
        return "rgb0";
    case PixelFormat::bgr0:
        return "bgr0";
    case PixelFormat::rgb24:
        return "rgb24";
    case PixelFormat::bgr24:
        return "bgr24";
    case PixelFormat::gray8:
        return "gray8";
    case PixelFormat::nv12:
        return "nv12";
    case PixelFormat::yuv420p:
        return "yuv420p";
    case PixelFormat::p010:
        return "p010";
    case PixelFormat::rgba64:
        return "rgba64";
    }

    return "unknown";
}

[[nodiscard]] constexpr std::string_view toString (ColorRange r) noexcept
{
    switch (r)
    {
    case ColorRange::unspecified:
        return "unspecified";
    case ColorRange::limited:
        return "limited";
    case ColorRange::full:
        return "full";
    }

    return "unknown";
}

/// Number of planes an Image in this format carries.
[[nodiscard]] constexpr int getPlaneCount (PixelFormat f) noexcept
{
    switch (f)
    {
    case PixelFormat::nv12:
    case PixelFormat::p010:
        return 2;
    case PixelFormat::yuv420p:
        return 3;
    default:
        return 1;
    }
}

/// True for the single-plane RGB layouts (8- and 16-bit).
[[nodiscard]] constexpr bool isPackedRgb (PixelFormat f) noexcept
{
    return f != PixelFormat::gray8 && f != PixelFormat::nv12 && f != PixelFormat::yuv420p && f != PixelFormat::p010;
}

/// True when the format stores chroma at half resolution (output dimensions must be even).
[[nodiscard]] constexpr bool hasChromaSubsampling (PixelFormat f) noexcept
{
    return f == PixelFormat::nv12 || f == PixelFormat::yuv420p || f == PixelFormat::p010;
}

/// Bits per sample component (8 for the classic formats, 10 for p010, 16 for rgba64).
[[nodiscard]] constexpr int getBitDepth (PixelFormat f) noexcept
{
    switch (f)
    {
    case PixelFormat::p010:
        return 10;
    case PixelFormat::rgba64:
        return 16;
    default:
        return 8;
    }
}

/// Bytes per sample cell in the given plane (e.g. 4 for rgba, 2 for the interleaved UV plane of
/// nv12).
[[nodiscard]] constexpr int getBytesPerPixel (PixelFormat f, int plane = 0) noexcept
{
    switch (f)
    {
    case PixelFormat::rgba:
    case PixelFormat::bgra:
    case PixelFormat::argb:
    case PixelFormat::abgr:
    case PixelFormat::rgb0:
    case PixelFormat::bgr0:
        return 4;
    case PixelFormat::rgb24:
    case PixelFormat::bgr24:
        return 3;
    case PixelFormat::gray8:
    case PixelFormat::yuv420p:
        return 1;
    case PixelFormat::nv12:
        return plane == 0 ? 1 : 2;
    case PixelFormat::p010:
        return plane == 0 ? 2 : 4;
    case PixelFormat::rgba64:
        return 8;
    }

    return 1;
}

/// log2 of the horizontal/vertical chroma subsampling factor for planes > 0.
[[nodiscard]] constexpr int getChromaShift (PixelFormat f) noexcept
{
    return hasChromaSubsampling (f) ? 1 : 0;
}

inline std::ostream& operator<< (std::ostream& os, PixelFormat f)
{
    return os << toString (f);
}

inline std::ostream& operator<< (std::ostream& os, ColorRange r)
{
    return os << toString (r);
}

} // namespace stills

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#include <format>
/// Every enum this library hands back formats, so a consumer's log line does not have to remember
/// which ones happened to have a formatter and which only an ostream inserter.
#define STILLS_DEFINE_ENUM_FORMATTER(Enum)                                                                             \
    template <>                                                                                                        \
    struct std::formatter<Enum> : std::formatter<std::string_view>                                                     \
    {                                                                                                                  \
        template <class Ctx>                                                                                           \
        auto format (Enum value, Ctx& ctx) const                                                                       \
        {                                                                                                              \
            return std::formatter<std::string_view>::format (stills::toString (value), ctx);                           \
        }                                                                                                              \
    }

STILLS_DEFINE_ENUM_FORMATTER (stills::PixelFormat);
STILLS_DEFINE_ENUM_FORMATTER (stills::ColorRange);
#endif

#pragma once
// stills/stills_Geometry.h — pixel-dimension value type. No FFmpeg dependency.

#include <compare>
#include <cstddef>
#include <functional>
#include <ostream>
#include <string>
#include <version>

#include "stills/detail/stills_Config.h"

#if STILLS_HAS_FORMAT
#include <format>
#endif

namespace stills
{

/// Width/height in pixels.
struct Size
{
    int width{ 0 };
    int height{ 0 };

    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr Size transposed() const noexcept { return { height, width }; }

    friend constexpr bool operator== (const Size&, const Size&) noexcept = default;
    /// Ordered by width, then height, so a Size can key an ordered container (a converter cache).
    friend constexpr std::strong_ordering operator<=> (const Size&, const Size&) noexcept = default;
};

[[nodiscard]] inline std::string toString (Size size)
{
    return std::to_string (size.width) + "x" + std::to_string (size.height);
}

inline std::ostream& operator<< (std::ostream& os, Size size)
{
    return os << toString (size);
}

} // namespace stills

template <>
struct std::hash<stills::Size>
{
    [[nodiscard]] constexpr std::size_t operator() (stills::Size size) const noexcept
    {
        return static_cast<std::size_t> (static_cast<unsigned int> (size.width)) * 0x9E3779B97F4A7C15ULL
               + static_cast<std::size_t> (static_cast<unsigned int> (size.height));
    }
};

#if STILLS_HAS_FORMAT
template <>
struct std::formatter<stills::Size> : std::formatter<std::string>
{
    template <class Ctx>
    auto format (stills::Size size, Ctx& ctx) const
    {
        return std::formatter<std::string>::format (stills::toString (size), ctx);
    }
};
#endif

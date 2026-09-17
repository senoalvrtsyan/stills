#pragma once
// stills/geometry.hpp — pixel-dimension value type. No FFmpeg dependency.

#include <compare>
#include <cstddef>
#include <functional>
#include <ostream>
#include <string>
#include <version>

#include "stills/detail/config.hpp"

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#include <format>
#endif

namespace stills {

/// Width/height in pixels.
struct Size {
  int width{0};
  int height{0};

  [[nodiscard]] constexpr bool is_empty() const noexcept { return width <= 0 || height <= 0; }
  [[nodiscard]] constexpr Size transposed() const noexcept { return {height, width}; }

  friend constexpr bool operator==(const Size&, const Size&) noexcept = default;
  /// Ordered by width, then height, so a Size can key an ordered container (a converter cache).
  friend constexpr std::strong_ordering operator<=>(const Size&, const Size&) noexcept = default;
};

[[nodiscard]] inline std::string to_string(Size s) {
  return std::to_string(s.width) + "x" + std::to_string(s.height);
}

inline std::ostream& operator<<(std::ostream& os, Size s) {
  return os << to_string(s);
}

}  // namespace stills

template <>
struct std::hash<stills::Size> {
  [[nodiscard]] constexpr std::size_t operator()(stills::Size s) const noexcept {
    return static_cast<std::size_t>(static_cast<unsigned>(s.width)) * 0x9E3779B97F4A7C15ULL +
           static_cast<std::size_t>(static_cast<unsigned>(s.height));
  }
};

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
template <>
struct std::formatter<stills::Size> : std::formatter<std::string> {
  template <class Ctx>
  auto format(stills::Size s, Ctx& ctx) const {
    return std::formatter<std::string>::format(stills::to_string(s), ctx);
  }
};
#endif

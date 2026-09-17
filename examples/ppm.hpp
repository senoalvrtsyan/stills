#pragma once
// Minimal binary PPM (P6) writer for the examples — keeps them dependency-free.
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>

inline bool write_ppm(const std::string& path, int width, int height, std::size_t row_stride,
                      std::span<const std::byte> rgb24) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", width, height);
  const auto row_bytes = static_cast<std::size_t>(width) * 3;
  for (int y = 0; y < height; ++y) {
    const std::byte* row = rgb24.data() + static_cast<std::size_t>(y) * row_stride;
    if (std::fwrite(row, 1, row_bytes, f) != row_bytes) {
      std::fclose(f);
      return false;
    }
  }
  return std::fclose(f) == 0;
}

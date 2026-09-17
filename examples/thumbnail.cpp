// thumbnail <input> <seconds> <out.ppm> [max_width]
// Synchronous extraction of one frame, written as a PPM.
#include <cstdlib>
#include <iostream>
#include <stills/stills.hpp>
#include <string>

#include "ppm.hpp"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: thumbnail <input> <seconds> <out.ppm> [max_width]\n";
    return 2;
  }
  stills::set_log_level(stills::LogLevel::error);

  stills::Options options;
  options.pixel_format = stills::PixelFormat::rgb24;
  if (argc > 4) options.maximum_size = stills::Size{std::atoi(argv[4]), 0};

  auto generator = stills::AssetImageGenerator::open(argv[1], options);
  if (!generator) {
    std::cerr << "open failed: " << generator.error() << '\n';
    return 1;
  }
  const auto& info = generator->info();
  std::cout << info.codec_name << ' ' << info.coded_size << " -> " << info.output_size
            << ", duration " << (info.duration ? to_string(*info.duration) : "unknown")
            << ", decoder " << (generator->active_decoder().hardware ? "hardware" : "software")
            << '\n';

  auto image = generator->image_at(stills::Time::seconds(std::atof(argv[2])));
  if (!image) {
    std::cerr << "extraction failed: " << image.error() << '\n';
    return 1;
  }
  std::cout << "frame at " << image->actual_time() << (image->is_keyframe() ? " (keyframe)" : "")
            << (image->was_clamped() ? " (clamped)" : "") << '\n';
  if (!write_ppm(argv[3], image->width(), image->height(), image->row_stride(0), image->pixels())) {
    std::cerr << "could not write " << argv[3] << '\n';
    return 1;
  }
  return 0;
}

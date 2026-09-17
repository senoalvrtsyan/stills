// contact_sheet <input> <cols> <rows> <out.ppm> [cell_width]
// Asynchronous batch extraction of cols*rows evenly spaced frames, tiled into one PPM.
// Ctrl-C cancels the batch; every pending frame is then reported as cancelled.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stills/stills.hpp>
#include <string>
#include <vector>

#include "ppm.hpp"

namespace {
std::atomic<bool> interrupted{false};
void on_sigint(int) {
  interrupted.store(true);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "usage: contact_sheet <input> <cols> <rows> <out.ppm> [cell_width]\n";
    return 2;
  }
  stills::set_log_level(stills::LogLevel::error);
  const int cols = std::atoi(argv[2]);
  const int rows = std::atoi(argv[3]);
  const int cell_w = argc > 5 ? std::atoi(argv[5]) : 160;

  stills::Options options;
  options.pixel_format = stills::PixelFormat::rgb24;
  options.maximum_size = stills::Size{cell_w, 0};
  options.tolerance = stills::Tolerance::exact();

  auto generator = stills::AssetImageGenerator::open(argv[1], options);
  if (!generator) {
    std::cerr << "open failed: " << generator.error() << '\n';
    return 1;
  }
  if (!generator->info().duration) {
    std::cerr << "asset duration unknown; cannot space frames\n";
    return 1;
  }
  const stills::Size cell = generator->info().output_size;
  const int sheet_w = cell.width * cols;
  const int sheet_h = cell.height * rows;
  std::vector<std::byte> sheet(static_cast<std::size_t>(sheet_w) *
                               static_cast<std::size_t>(sheet_h) * 3);

  const int count = cols * rows;
  const double duration = generator->info().duration->to_seconds();
  std::vector<stills::Time> times;
  for (int i = 0; i < count; ++i)
    times.push_back(stills::Time::seconds(duration * (i + 0.5) / count));

  std::mutex io;
  int ok = 0;
  int failed = 0;
  int cancelled = 0;
  auto request = generator->generate_images(times, [&](stills::Completion c) {
    std::lock_guard lk(io);
    switch (c.status()) {
      case stills::GenerationStatus::succeeded: {
        ++ok;
        const int slot = static_cast<int>(c.index);  // position in `times`
        const stills::Image& img = *c.result;
        const int x0 = (slot % cols) * cell.width;
        const int y0 = (slot / cols) * cell.height;
        for (int y = 0; y < img.height(); ++y) {
          const std::byte* src =
              img.pixels().data() + static_cast<std::size_t>(y) * img.row_stride(0);
          std::byte* dst =
              sheet.data() + (static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(sheet_w) +
                              static_cast<std::size_t>(x0)) *
                                 3;
          std::memcpy(dst, src, static_cast<std::size_t>(img.width()) * 3);
        }
        std::cout << "  " << c.requested_time << " -> " << img.actual_time() << '\n';
        break;
      }
      case stills::GenerationStatus::failed:
        ++failed;
        std::cerr << "  " << c.requested_time << " failed: " << c.result.error() << '\n';
        break;
      case stills::GenerationStatus::cancelled:
        ++cancelled;
        break;
    }
  });

  std::signal(SIGINT, on_sigint);
  while (request.wait_for(std::chrono::milliseconds{50}) != stills::WaitResult::finished) {
    if (interrupted.load()) {
      std::cerr << "interrupted: cancelling\n";
      generator->cancel_all();
    }
  }
  std::cout << ok << " ok, " << failed << " failed, " << cancelled << " cancelled\n";
  if (ok == 0) return 1;
  if (!write_ppm(argv[4], sheet_w, sheet_h, static_cast<std::size_t>(sheet_w) * 3, sheet)) {
    std::cerr << "could not write " << argv[4] << '\n';
    return 1;
  }
  return 0;
}

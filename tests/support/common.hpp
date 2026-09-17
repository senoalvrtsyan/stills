#pragma once
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <stills/stills.hpp>

#include "support/collector.hpp"
#include "support/expect.hpp"
#include "support/fixtures.hpp"
#include "support/frame_index.hpp"

namespace testsupport {

/// Software-only options with the given output format. One decoder thread keeps the suite
/// deterministic and race-detector friendly; the library default is libavcodec's automatic count.
inline stills::Options sw_options(stills::PixelFormat fmt = stills::PixelFormat::yuv420p) {
  stills::Options o;
  o.hardware.policy = stills::HardwarePolicy::software_only;
  o.pixel_format = fmt;
  o.decoder_threads = 1;
  return o;
}

/// A generator over counter.mp4 (software, yuv420p by default).
inline stills::AssetImageGenerator open_counter(stills::Options o = sw_options()) {
  auto g = stills::AssetImageGenerator::open(fixture("counter.mp4").string(), std::move(o));
  if (!g) FAIL("open(counter.mp4) failed: " << g.error());
  return std::move(*g);
}

/// Quiet libav once per test binary.
struct QuietLogs {
  QuietLogs() { stills::set_log_level(stills::LogLevel::quiet); }
};
inline const QuietLogs quiet_logs{};

}  // namespace testsupport

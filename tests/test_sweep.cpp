// Random-order correctness sweep: every fixture, seeded random exact requests in exact and in
// keyframe mode. The expected frame comes from a sequential exact scan of the same fixture
// (cross-checked against the fixture's luma formula where one exists), so variable frame rates and
// edit lists are covered too.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <random>
#include <stills/interop.hpp>
#include <string>
#include <thread>
#include <vector>

#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::Time;
using stills::Tolerance;

namespace {

struct RefFrame {
  int index;    // luma index
  Time actual;  // presentation time (asset-relative)
  bool key;
  int first_tick;  // first 1/30 s request tick this frame was returned for
};

/// Exact sequential scan of `name`: one request per 1/30 s tick. Frames are keyed by the request
/// tick that first returned them, so containers that round presentation times to their own tick
/// (Matroska: 1 ms) compare the way the library itself compares (nearest tick), not by exact Time.
struct Reference {
  std::vector<RefFrame> frames;   // presentation order, deduplicated
  std::vector<std::size_t> tick;  // tick n -> position in `frames`
  int last_tick{0};               // last 1/30 s tick that produced a frame

  static Reference scan(const std::string& name, int ticks) {
    Reference r;
    auto g = REQUIRE_OK(AssetImageGenerator::open(fixture(name).string(), sw_options()));
    for (int n = 0; n < ticks; ++n) {
      auto img = g.image_at(Time{n, 30});
      if (!img) {
        if (img.error().code == ErrorCode::time_out_of_range) break;
        FAIL("reference scan of " << name << " at tick " << n << ": " << img.error());
      }
      r.last_tick = n;
      if (r.frames.empty() || r.frames.back().actual != img->actual_time()) {
        r.frames.push_back({frame_index_of(*img), img->actual_time(), img->is_keyframe(), n});
      }
      r.tick.push_back(r.frames.size() - 1);
    }
    REQUIRE(r.frames.size() >= 2);
    return r;
  }
  /// The frame on screen at request time `t` (a tick or a half tick; frames never start on half
  /// ticks).
  [[nodiscard]] const RefFrame& exact_at(Time t) const {
    const auto n = static_cast<std::size_t>(
        t.to_timestamp(stills::Rational{1, 30}, stills::TimeRounding::down));
    return frames[tick[std::min(n, tick.size() - 1)]];
  }
  /// The keyframe at or before `t`, or the first presented frame when no keyframe precedes `t`.
  [[nodiscard]] std::pair<const RefFrame*, bool> key_at(Time t) const {
    const int n =
        static_cast<int>(t.to_timestamp(stills::Rational{1, 30}, stills::TimeRounding::down));
    const RefFrame* best = nullptr;
    for (const auto& f : frames)
      if (f.key && f.first_tick <= n) best = &f;
    if (best != nullptr) return {best, false};
    return {&frames.front(), true};
  }
  /// The frame following `f` in presentation order (nullptr at the end).
  [[nodiscard]] const RefFrame* after(const RefFrame& f) const {
    for (std::size_t i = 0; i + 1 < frames.size(); ++i)
      if (frames[i].actual == f.actual) return &frames[i + 1];
    return nullptr;
  }
  [[nodiscard]] const RefFrame* by_time(Time actual) const {
    for (const auto& f : frames)
      if (f.actual == actual) return &f;
    return nullptr;
  }
};

struct Fixture {
  const char* name;
  int ticks;  // request range in 1/30 s ticks (the clip length)
  std::function<int(int)>
      luma_formula;  // luma index of the frame on screen at tick n; null = reference only
  bool available{true};
};

const std::vector<Fixture>& fixtures() {
  static const std::vector<Fixture> all = {
      {"counter.mp4", 120, [](int n) { return n; }},
      {"counter_offset.ts", 120, [](int n) { return n; }},
      {"counter.mkv", 120, [](int n) { return n; }},
      {"counter_frag.mp4", 120, [](int n) { return n; }},
      {"counter_opengop.mp4", 120, [](int n) { return n; }},
      {"counter_opengop_hevc.mp4", 120, [](int n) { return n; }, STILLS_HAVE_X265 != 0},
      {"counter_ilace.mp4", 120, [](int n) { return n; }},
      {"counter_longgop.ts", 120, [](int n) { return n; }},
      {"counter_vfr.mp4", 120, [](int n) { return n - n % 3; }},
      {"counter_editlist.mp4", 87, [](int n) { return n + 33; }},
      {"counter_vfr_hold.mp4", 600, nullptr},
  };
  return all;
}

/// Request times: frame ticks and half ticks (the latter exercise the display-interval logic of
/// the held frame and the NONREF gap handling).
Time random_time(std::mt19937& rng, int last_tick) {
  const int n = std::uniform_int_distribution<int>(0, last_tick)(rng);
  const int half = std::uniform_int_distribution<int>(0, 1)(rng);
  return Time{2 * n + half, 60};
}

struct SweepResult {
  int wrong{0}, errors{0};
  std::string first_wrong;
};

SweepResult sweep(const std::string& name, const Reference& ref, stills::Options o,
                  bool keyframe_mode, int requests, unsigned seed) {
  std::mt19937 rng(seed);
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture(name).string(), o));
  SweepResult r;
  for (int i = 0; i < requests; ++i) {
    const Time t = random_time(rng, ref.last_tick);
    auto img = g.image_at(t);
    if (!img) {
      ++r.errors;
      if (r.first_wrong.empty())
        r.first_wrong = "request " + to_string(t) + " failed: " + to_string(img.error());
      continue;
    }
    const int idx = frame_index_of(*img);
    int expected = 0;
    bool clamped_ok = false;
    if (keyframe_mode) {
      const auto [kf, clamped] = ref.key_at(t);
      expected = kf->index;
      clamped_ok = clamped;
      if (!clamped && !img->is_keyframe() && r.first_wrong.empty()) {
        r.first_wrong =
            "request " + to_string(t) + " returned a non-keyframe " + std::to_string(idx);
      }
    } else {
      expected = ref.exact_at(t).index;
    }
    if (idx != expected || (keyframe_mode && clamped_ok && !img->was_clamped())) {
      ++r.wrong;
      if (r.first_wrong.empty()) {
        r.first_wrong = "request " + to_string(t) + " got " + std::to_string(idx) + " at " +
                        to_string(img->actual_time()) + " expected " + std::to_string(expected);
      }
    }
  }
  return r;
}

}  // namespace

TEST_CASE("sweep: random-order requests return the exact frame", "[sweep][slow]") {
  for (const Fixture& f : fixtures()) {
    if (!f.available) continue;
    DYNAMIC_SECTION(f.name) {
      const Reference ref = Reference::scan(f.name, f.ticks);
      if (f.luma_formula) {
        // The reference itself agrees with the fixture's luma formula at every tick.
        for (int n = 0; n <= ref.last_tick; ++n) {
          const int expected = f.luma_formula(n);
          const int got = ref.exact_at(Time{n, 30}).index;
          if (got != expected)
            FAIL(f.name << ": reference frame at tick " << n << " is " << got << ", formula says "
                        << expected);
        }
      }
      const SweepResult exact = sweep(f.name, ref, sw_options(), false, 400, 11);
      INFO(f.name << " exact: " << exact.first_wrong);
      CHECK(exact.wrong == 0);
      CHECK(exact.errors == 0);

      stills::Options any = sw_options();
      any.tolerance = Tolerance::any();
      const SweepResult key = sweep(f.name, ref, any, true, 400, 13);
      INFO(f.name << " any(): " << key.first_wrong);
      CHECK(key.wrong == 0);
      CHECK(key.errors == 0);
    }
  }
}

// The same random-order sweep while a second thread floods the generator with batches and cancels
// them. A request abandoned part-way leaves the decoder's forward state describing frames it never
// produced, so every answer here has to come from a repositioned decoder rather than that state.
TEST_CASE("sweep: random-order requests stay exact while batches are cancelled",
          "[sweep][cancel][slow]") {
  for (const Fixture& f : fixtures()) {
    if (!f.available) continue;
    DYNAMIC_SECTION(f.name) {
      const Reference ref = Reference::scan(f.name, f.ticks);
      auto g = REQUIRE_OK(AssetImageGenerator::open(fixture(f.name).string(), sw_options()));
      std::atomic<bool> stop{false};
      std::thread flooder([&] {
        std::mt19937 rng(4242);
        while (!stop.load()) {
          std::vector<Time> times;
          for (int i = 0; i < 4; ++i) times.push_back(random_time(rng, ref.last_tick));
          auto req = g.generate_images(times, [](stills::Completion) {});
          std::this_thread::yield();
          req.cancel();
          (void)req.wait_for(std::chrono::seconds{30});
        }
      });
      std::mt19937 rng(2024);
      int wrong = 0, errors = 0;
      std::string first_wrong;
      for (int i = 0; i < 300; ++i) {
        const Time t = random_time(rng, ref.last_tick);
        auto img = g.image_at(t);
        if (!img) {
          ++errors;
          if (first_wrong.empty())
            first_wrong = "request " + to_string(t) + " failed: " + to_string(img.error());
          continue;
        }
        const int expected = ref.exact_at(t).index;
        if (frame_index_of(*img) != expected) {
          ++wrong;
          if (first_wrong.empty()) {
            first_wrong = "request " + to_string(t) + " got " +
                          std::to_string(frame_index_of(*img)) + " expected " +
                          std::to_string(expected) + " at " + to_string(img->actual_time());
          }
        }
      }
      stop.store(true);
      flooder.join();
      INFO(f.name << ": " << first_wrong);
      CHECK(wrong == 0);
      CHECK(errors == 0);
    }
  }
}

// The same sweep on a hardware decoder, against a reference scanned in software: a decoder that
// returns the neighbouring frame, or the right frame with the wrong timestamp, is the one failure
// mode the rest of the hardware tests (which check a handful of times each) would not catch. The
// name begins "hw" so the tsan preset's `stills\.hw` exclusion covers it: ThreadSanitizer's own
// runtime aborts inside the NVIDIA driver.
TEST_CASE("hw: the random-order sweep agrees with software on a hardware decoder",
          "[hw][sweep][slow]") {
  const auto devices = stills::interop::available_device_types();
  if (devices.empty()) SKIP("no hardware device can be created on this machine");

  stills::Options hw;
  hw.hardware.policy = stills::HardwarePolicy::require_hardware;
  hw.pixel_format = stills::PixelFormat::yuv420p;

  std::vector<std::string> covered;
  for (const Fixture& f : fixtures()) {
    if (!f.available) continue;
    // Not every fixture has a hardware decoder here (codec, profile, or a frame size the driver
    // refuses); those are not failures, they are simply not part of this sweep.
    if (!AssetImageGenerator::open(fixture(f.name).string(), hw)) continue;
    covered.push_back(f.name);
  }
  if (covered.empty()) {
    SKIP("no fixture opens with require_hardware here (" << devices.size()
                                                         << " device types available)");
  }

  for (const std::string& name : covered) {
    DYNAMIC_SECTION(name) {
      const Fixture* f = nullptr;
      for (const Fixture& c : fixtures()) {
        if (c.name == name) f = &c;
      }
      REQUIRE(f != nullptr);
      const Reference ref = Reference::scan(name, f->ticks);  // software: the thing to agree with

      const SweepResult exact = sweep(name, ref, hw, false, 200, 23);
      INFO(name << " hardware, exact: " << exact.first_wrong);
      CHECK(exact.wrong == 0);
      CHECK(exact.errors == 0);

      stills::Options any = hw;
      any.tolerance = Tolerance::any();
      const SweepResult key = sweep(name, ref, any, true, 200, 31);
      INFO(name << " hardware, any(): " << key.first_wrong);
      CHECK(key.wrong == 0);
      CHECK(key.errors == 0);
    }
  }
}

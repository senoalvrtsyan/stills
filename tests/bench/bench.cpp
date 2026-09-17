// tests/bench/bench.cpp — the before/after measuring stick for the pipeline restructure.
//
// Not a test and not registered with ctest: it has no pass/fail, it produces numbers that only mean
// something next to the same numbers from another revision. Run it by hand, on the release preset,
// on an otherwise idle machine:
//
//     cmake --build --preset release --target stills_bench
//     ./build/release/tests/stills_bench
//
// What it measures, and why that shape:
//
//   * Software decoding only. A hardware path makes the numbers a property of the driver rather
//     than of this library, and unreproducible on the reviewer's machine.
//   * One decoder thread. The counts below have to be reproducible run to run, and the comparison
//     is between two revisions on one machine, not a throughput claim.
//   * detail::Pipeline directly, not AssetImageGenerator: the async engine's queueing and thread
//     handoff would be measured along with the decode, and none of it is being restructured.
//     tests/test_cancel_io.cpp already drives the pipeline this way.
//   * Two request orders per case. `sweep` walks forward and is served largely by continuing from
//     the held frame — the invariant the FrameSlot/DecodeFrontier extraction moves. `scatter` jumps
//     and is served by the positioner. One order alone would leave half the restructure unmeasured.
//
// **Seeks and decoded frames are the real output.** Milliseconds are a property of the machine and
// the hour; the counts are a property of the code. A restructure that changes which seeks happen is
// a behaviour change wearing a refactor's clothes, and it shows up here as a changed count long
// before it shows up as a changed millisecond. Both counters already exist in the shipped library
// for SeekCostModel and forward_is_cheaper(); this only reads them (Pipeline::getSeekCount).
//
// How to compare two revisions, because the milliseconds do not survive being compared across
// sittings. These fixtures are small and a request costs tens of microseconds, so this column
// measures per-request overhead rather than decode throughput, and it is dominated by whatever else
// the machine is doing: measured here, the same binary summed 1.32-1.43 ms on an idle machine and
// 2.4-4.7 ms under a load average of 15. Do not compare a number taken today against one written
// down last week. Build the other revision in a worktree, drop this file and its five CMake lines
// into it, and run the two binaries alternately in one sitting, taking the median of several
// rounds. Even then, treat a time difference under about 10% as nothing.
//
// Going *backwards* takes one more thing: Pipeline::getSeekCount and getDecodedFrameCount arrived
// with this file, so a revision older than it has neither, and the file will not compile there
// until the two accessors are dropped in as well. The pre-restructure baseline is therefore taken
// from the revision that introduced this harness, not from the one before it -- that step changes
// no library behaviour, so the two are the same asset to measure.
//
// One caveat on those counts, and it is the reason every case is run more than once. The
// seek-versus-decode-forward decision is not purely structural: forward_is_cheaper() compares
// `frames_to_key * frameCostMs` against `seekCostMs`, and both are exponential averages of
// measured wall-clock time. A request sitting near that boundary can go either way on a loaded
// machine. So each case is run `--reps` times from a fresh pipeline, and a count that is not
// identical across all reps is printed as `a|b|c` rather than a single number. Those cases are the
// ones where a later "the count changed" needs more than one run to mean anything.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <stills/detail/pipeline.hpp>
#include <stills/stills.hpp>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view fixtures[] = {
    // An MP4 with a trusted index, a long-GOP MPEG-TS that has to scan, a Matroska, and 10-bit
    // HEVC — one file for each positioning strategy the pipeline can take, plus one that makes the
    // per-frame conversion cost visible.
    "counter.mp4", "counter_longgop.ts", "counter.mkv", "counter_p10.mp4",
};

struct Mode {
  std::string_view name;
  stills::Tolerance tolerance;
};

const Mode modes[] = {
    {"exact", stills::Tolerance::exact()},
    {"keyframe", stills::Tolerance::any()},
};

/// Ascending: 0, 1, 2, ... Served by continuing from the held frame wherever the pipeline can.
std::vector<int> sweepOrder(int n) {
  std::vector<int> out(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) out[static_cast<std::size_t>(i)] = i;
  return out;
}

/// Bit-reversed over the next power of two, dropping what falls past `n`: a deterministic,
/// RNG-free permutation that is spread over the whole asset and jumps both ways every step.
std::vector<int> scatterOrder(int n) {
  int bits = 1;
  while ((1 << bits) < n) ++bits;
  std::vector<int> out;
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < (1 << bits); ++i) {
    int r = 0;
    for (int b = 0; b < bits; ++b) {
      if ((i & (1 << b)) != 0) r |= 1 << (bits - 1 - b);
    }
    if (r < n) out.push_back(r);
  }
  return out;
}

struct Order {
  std::string_view name;
  std::vector<int> (*build)(int);
};

const Order orders[] = {{"sweep", &sweepOrder}, {"scatter", &scatterOrder}};

/// One run of one case from a fresh pipeline.
struct Run {
  double openMs{0};
  double requestMs{0};  ///< summed over every request, open excluded
  long seeks{0};
  long framesDecoded{0};
};

/// Every rep of one case. Times are summarised by the median; counts are reported per rep, because
/// a count that moves is the thing worth seeing, not a count that averages.
struct Case {
  std::string_view fixture;
  std::string_view mode;
  std::string_view order;
  std::vector<Run> runs;
};

[[nodiscard]] double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

/// "12" when every rep agreed, "12|13|12" when they did not.
[[nodiscard]] std::string counts(const std::vector<Run>& runs, long Run::* field) {
  const bool stable = std::ranges::all_of(
      runs, [&](const Run& r) { return r.*field == runs.front().*field; });
  if (stable) return std::to_string(runs.front().*field);
  std::string s;
  for (const Run& r : runs) {
    if (!s.empty()) s += '|';
    s += std::to_string(r.*field);
  }
  return s;
}

[[nodiscard]] std::string fixturePath(std::string_view name) {
  return std::string{STILLS_FIXTURE_DIR} + "/" + std::string{name};
}

/// Opens the asset, issues `order.size()` requests at times spread over its duration, and returns
/// what that cost. `requested[i] = duration * order[i] / count`, so the last request stays inside
/// the asset and OutOfRangePolicy::error never fires.
[[nodiscard]] Run runOnce(std::string_view fixture, const Mode& mode,
                          const std::vector<int>& order) {
  stills::Options options;
  options.hardware.policy = stills::HardwarePolicy::software_only;
  options.tolerance = mode.tolerance;
  options.decoder_threads = 1;

  const stills::detail::CancelToken token;
  Run run;

  const auto openStart = Clock::now();
  auto pipeline = stills::detail::Pipeline::open(fixturePath(fixture), options);
  const auto openEnd = Clock::now();
  if (!pipeline) {
    std::cerr << "stills_bench: open(" << fixture << ") failed: " << pipeline.error() << "\n";
    std::exit(1);
  }
  run.openMs = std::chrono::duration<double, std::milli>(openEnd - openStart).count();

  const auto duration = (*pipeline)->info().duration;
  if (!duration || !duration->is_finite()) {
    std::cerr << "stills_bench: " << fixture << " declares no usable duration\n";
    std::exit(1);
  }
  const std::int64_t span = duration->value();
  const auto count = static_cast<std::int64_t>(order.size());

  for (const int index : order) {
    const stills::Time requested{span * index / count, duration->timescale()};
    const auto start = Clock::now();
    auto image = (*pipeline)->image_at(requested, token);
    const auto end = Clock::now();
    if (!image) {
      std::cerr << "stills_bench: " << fixture << " image_at(" << stills::to_string(requested)
                << ") failed: " << image.error() << "\n";
      std::exit(1);
    }
    run.requestMs += std::chrono::duration<double, std::milli>(end - start).count();
    run.seeks += (*pipeline)->getSeekCount();
    run.framesDecoded += (*pipeline)->getDecodedFrameCount();
  }
  return run;
}

void report(const std::vector<Case>& cases, int requests) {
  std::cout << std::format("\n{:<20} {:<9} {:<8} {:>9} {:>9} {:>10} {:>12}\n", "fixture", "mode",
                           "order", "open ms", "ms/req", "seeks", "frames")
            << std::string(82, '-') << "\n";
  double totalMs = 0;
  long totalSeeks = 0;
  long totalFrames = 0;
  for (const Case& c : cases) {
    std::vector<double> openMs;
    std::vector<double> perRequest;
    for (const Run& r : c.runs) {
      openMs.push_back(r.openMs);
      perRequest.push_back(r.requestMs / requests);
    }
    const double medianPerRequest = median(perRequest);
    std::cout << std::format("{:<20} {:<9} {:<8} {:>9.2f} {:>9.3f} {:>10} {:>12}\n", c.fixture,
                             c.mode, c.order, median(openMs), medianPerRequest,
                             counts(c.runs, &Run::seeks), counts(c.runs, &Run::framesDecoded));
    totalMs += medianPerRequest;
    totalSeeks += c.runs.front().seeks;
    totalFrames += c.runs.front().framesDecoded;
  }
  // The times are summed, not averaged: one number per revision to compare, with the same weight
  // on every case. The counts are rep 1's, which is all of them whenever nothing printed `a|b|c`.
  std::cout << std::string(82, '-') << "\n"
            << std::format("{:<39} {:>19.3f} {:>10} {:>12}\n", "sum over all cases", totalMs,
                           totalSeeks, totalFrames);
}

}  // namespace

int main(int argc, char** argv) {
  int reps = 3;
  int requests = 40;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    const auto value = [&]() -> int {
      if (i + 1 >= argc) {
        std::cerr << "stills_bench: " << arg << " needs a value\n";
        std::exit(2);
      }
      return std::atoi(argv[++i]);
    };
    if (arg == "--reps") {
      reps = value();
    } else if (arg == "--requests") {
      requests = value();
    } else {
      std::cerr << "usage: stills_bench [--reps N] [--requests N]\n";
      return 2;
    }
  }
  if (reps < 1 || requests < 2) {
    std::cerr << "stills_bench: --reps must be >= 1 and --requests >= 2\n";
    return 2;
  }

  stills::set_log_level(stills::LogLevel::quiet);

  std::cout << std::format(
      "stills_bench: {} build, software only, 1 decoder thread, {} reps x {} requests\n"
      "counts are per rep; `a|b|c` means the reps disagreed (see the header comment)\n",
#ifdef NDEBUG
      "release",
#else
      "debug",
#endif
      reps, requests);

  std::vector<Case> cases;
  for (const std::string_view fixture : fixtures) {
    for (const Mode& mode : modes) {
      for (const Order& order : orders) {
        const std::vector<int> indices = order.build(requests);
        Case c{fixture, mode.name, order.name, {}};
        for (int rep = 0; rep < reps; ++rep) c.runs.push_back(runOnce(fixture, mode, indices));
        cases.push_back(std::move(c));
      }
    }
  }
  report(cases, requests);
  return 0;
}

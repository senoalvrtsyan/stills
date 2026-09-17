// Randomised concurrent soak. The rest of the suite holds at most three generators at once and
// drives each one deliberately; this holds eight and drives them at random from eight threads
// while destroying and reopening them underneath, which is the shape a video-editor server has and
// the one that finds lifetime bugs.
//
// Each slot holds its generator behind a shared_mutex — shared to use it, exclusive to destroy and
// reopen it — so no thread ever calls a method on a generator another thread is destroying (that
// would be caller UB, not a library bug).
//
// The default run is a few seconds so it stays in the suite; STILLS_SOAK_SECONDS raises it for a
// real soak.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::AsyncRequest;
using stills::Completion;
using stills::Options;
using stills::RequestOptions;
using stills::Time;
using stills::Tolerance;

namespace {

constexpr int kSlots = 8;
constexpr int kThreads = 8;

const char* const kFixtures[] = {"counter.mp4",          "counter.mkv",         "counter_offset.ts",
                                 "counter_vfr.mp4",      "counter_opengop.mp4", "counter_frag.mp4",
                                 "counter_editlist.mp4", "counter_longgop.ts"};

int soak_seconds() {
  const char* env = std::getenv("STILLS_SOAK_SECONDS");
  const int n = env != nullptr ? std::atoi(env) : 0;
  return n > 0 ? n : 3;
}

Options soak_options(std::mt19937& rng) {
  Options o = sw_options(rng() % 3 == 0 ? stills::PixelFormat::rgba : stills::PixelFormat::yuv420p);
  if (rng() % 4 == 0) o.tolerance = Tolerance::any();
  return o;
}

std::unique_ptr<AssetImageGenerator> open_one(std::mt19937& rng, int slot) {
  const char* name = kFixtures[(static_cast<std::size_t>(slot) + rng() % std::size(kFixtures)) %
                               std::size(kFixtures)];
  auto g = AssetImageGenerator::open(fixture(name).string(), soak_options(rng));
  if (!g) return nullptr;
  return std::make_unique<AssetImageGenerator>(std::move(*g));
}

struct Slot {
  std::shared_mutex mutex;
  std::unique_ptr<AssetImageGenerator> generator;
};

/// One batch's accounting, shared with its handler so it outlives the loop iteration that made it.
struct BatchTrack {
  std::atomic<int> delivered{0};
  std::size_t expected = 0;
};

struct Totals {
  std::atomic<long> images{0}, errors{0}, batches{0}, completions{0};
  std::atomic<long> reopens{0};
  std::atomic<long> open_failures{0};
  std::atomic<long> miscounted{0};
  std::atomic<long> first_bad_expected{0}, first_bad_got{0};
};

}  // namespace

TEST_CASE("soak: eight generators, eight threads, destroyed and reopened underneath",
          "[soak][async][lifetime]") {
  const int seconds = soak_seconds();
  INFO("STILLS_SOAK_SECONDS=" << seconds);
  Slot slots[kSlots];
  Totals t;
  {
    std::mt19937 rng(1);
    for (int i = 0; i < kSlots; ++i) {
      slots[i].generator = open_one(rng, i);
      REQUIRE(slots[i].generator != nullptr);
    }
  }

  const auto worker = [&](int id) {
    std::mt19937 rng(77u + static_cast<unsigned>(id));
    std::vector<std::pair<AsyncRequest, std::shared_ptr<BatchTrack>>> pending;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
      Slot& s = slots[rng() % kSlots];
      const int op = static_cast<int>(rng() % 100);
      if (op < 40) {  // synchronous request
        std::shared_lock lk(s.mutex);
        if (!s.generator) continue;
        if (s.generator->image_at(Time{static_cast<int>(rng() % 125), 30})) {
          ++t.images;
        } else {
          ++t.errors;
        }
      } else if (op < 60) {  // a batch, sometimes cancelled, whose handler re-enters the generator
        std::shared_lock lk(s.mutex);
        if (!s.generator) continue;
        const int n = 1 + static_cast<int>(rng() % 12);
        std::vector<Time> times;
        for (int i = 0; i < n; ++i) times.push_back(Time{static_cast<int>(rng() % 125), 30});
        auto track = std::make_shared<BatchTrack>();
        track->expected = times.size();
        AssetImageGenerator* g = s.generator.get();
        const bool reenter = rng() % 3 == 0;
        auto req = s.generator->generate_images(
            times,
            [track, g, reenter](Completion c) {
              ++track->delivered;
              // Documented as permitted: a handler may call back into its own generator.
              if (reenter && c.index == 0) {
                (void)g->image_at(Time{5, 30});
                g->cancel_all();
              }
            });
        ++t.batches;
        if (rng() % 3 == 0) req.cancel();
        pending.emplace_back(std::move(req), std::move(track));
      } else if (op < 90) {  // churn: destroy and reopen the slot
        std::unique_lock lk(s.mutex);
        s.generator.reset();  // joins the worker; pending items are delivered `cancelled`
        s.generator = open_one(rng, id);
        if (!s.generator) ++t.open_failures;
        ++t.reopens;
      } else {
        std::shared_lock lk(s.mutex);
        if (!s.generator) continue;
        s.generator->cancel_all();
      }

      for (auto it = pending.begin(); it != pending.end();) {
        if (!it->first.finished()) {
          ++it;
          continue;
        }
        const int got = it->second->delivered.load();
        // Exactly one completion per requested time. The one documented exception: a batch queued
        // as teardown began is finished without any delivery, so zero is allowed too.
        if (got != static_cast<int>(it->second->expected) && got != 0) {
          if (t.miscounted.fetch_add(1) == 0) {
            t.first_bad_expected.store(static_cast<long>(it->second->expected));
            t.first_bad_got.store(got);
          }
        }
        t.completions += got;
        it = pending.erase(it);
      }
      if (pending.size() > 64) pending.front().first.wait();
    }
    for (auto& p : pending) (void)p.first.wait_for(std::chrono::seconds{30});
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker, i);
  for (auto& th : threads) th.join();
  for (auto& s : slots) s.generator.reset();

  INFO("images " << t.images << " errors " << t.errors << " batches " << t.batches
                 << " completions " << t.completions << " reopens " << t.reopens);
  INFO("first miscounted batch: expected " << t.first_bad_expected << " completions, got "
                                           << t.first_bad_got);
  CHECK(t.miscounted == 0);
  CHECK(t.open_failures == 0);
  // The run has to have actually done something, or the assertions above are vacuous.
  CHECK(t.images > 0);
  CHECK(t.batches > 0);
  CHECK(t.reopens > 0);
}

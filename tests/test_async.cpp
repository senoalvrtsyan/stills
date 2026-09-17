#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <latch>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "support/common.hpp"

using namespace testsupport;
using namespace std::chrono_literals;
using stills::AssetImageGenerator;
using stills::AsyncRequest;
using stills::Completion;
using stills::GenerationStatus;
using stills::Time;
using stills::WaitResult;

namespace {
std::vector<Time> frames(std::initializer_list<int> ns) {
  std::vector<Time> v;
  for (int n : ns) v.emplace_back(n, 30);
  return v;
}
std::vector<Time> range(int from, int to) {
  std::vector<Time> v;
  for (int n = from; n < to; ++n) v.emplace_back(n, 30);
  return v;
}

/// Live threads in this process, or nullopt where that cannot be asked. /proc/self/task is the
/// only portable-enough answer; there is no equivalent on macOS without <mach/…>, so the test that
/// uses this reports itself satisfied rather than failing on a platform it cannot measure.
std::optional<std::size_t> live_threads() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path task{"/proc/self/task"};
  if (!fs::is_directory(task, ec) || ec) return std::nullopt;
  std::size_t n = 0;
  for (fs::directory_iterator it(task, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
    ++n;
  if (ec) return std::nullopt;
  return n;
}
}  // namespace

TEST_CASE("async: exactly-once, in-order delivery on the worker thread", "[async]") {
  auto g = open_counter();
  Collector col;
  const auto times = frames({5, 90, 3, 31, 119, 0, 60, 60, 29, 45});
  AsyncRequest req = g.generate_images(times, col.handler());
  REQUIRE(req.valid());
  CHECK(req.size() == times.size());
  REQUIRE(req.wait_for(30s) == WaitResult::finished);
  CHECK(req.finished());
  CHECK(req.completed() == times.size());
  const auto items = col.items();
  REQUIRE(items.size() == times.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    CHECK(items[i].position == i);
    CHECK(items[i].requested == times[i]);
    CHECK(items[i].status == GenerationStatus::succeeded);
    REQUIRE(items[i].index.has_value());
    CHECK(*items[i].index == frame_index_of_time(times[i]));
    CHECK(items[i].actual == times[i]);
    CHECK(items[i].thread != std::this_thread::get_id());
  }
}

TEST_CASE("async: failures are delivered as failed, not dropped", "[async]") {
  auto g = open_counter();
  Collector col;
  AsyncRequest req =
      g.generate_images({Time{1, 30}, Time{99, 1}, Time{-1, 1}, Time{2, 30}}, col.handler());
  REQUIRE(req.wait_for(30s) == WaitResult::finished);
  const auto items = col.items();
  REQUIRE(items.size() == 4);
  CHECK(items[0].status == GenerationStatus::succeeded);
  CHECK(items[1].status == GenerationStatus::failed);
  CHECK(items[1].error == stills::ErrorCode::time_out_of_range);
  CHECK(items[2].status == GenerationStatus::failed);
  CHECK(items[2].error == stills::ErrorCode::invalid_argument);
  CHECK(items[3].status == GenerationStatus::succeeded);
}

TEST_CASE("async: cancel after some deliveries", "[async][cancel]") {
  auto g = open_counter();
  Collector col;
  std::latch third_delivered{1};
  std::latch cancel_issued{1};
  std::atomic<int> delivered{0};
  const auto times = range(0, 60);
  AsyncRequest req = g.generate_images(times, [&](Completion c) {
    col.add(std::move(c));
    if (++delivered == 3) {
      third_delivered.count_down();
      cancel_issued.wait();  // hold the worker inside this handler until cancel() has landed
    }
  });
  third_delivered.wait();
  req.cancel();
  req.cancel();  // idempotent
  cancel_issued.count_down();
  REQUIRE(req.wait_for(30s) == WaitResult::finished);
  const auto items = col.items();
  REQUIRE(items.size() == times.size());
  CHECK(col.count(GenerationStatus::failed) == 0);
  CHECK(col.count(GenerationStatus::succeeded) ==
        3);  // deterministic: cancel landed before item 4 started
  INFO(col.first_wrong());
  CHECK(col.all_exact());  // the frames, not just the tally
  CHECK(col.count(GenerationStatus::cancelled) == times.size() - 3);
  for (std::size_t i = 0; i < 3; ++i) CHECK(items[i].status == GenerationStatus::succeeded);
  for (std::size_t i = 3; i < items.size(); ++i)
    CHECK(items[i].status == GenerationStatus::cancelled);
  for (std::size_t i = 0; i < items.size(); ++i) CHECK(items[i].requested == times[i]);
}

TEST_CASE("async: a queued batch cancelled before it starts is fully cancelled",
          "[async][cancel]") {
  auto g = open_counter();
  Collector first;
  Collector second;
  AsyncRequest a = g.generate_images(range(0, 40), first.handler());
  AsyncRequest b = g.generate_images(range(0, 5), second.handler());
  b.cancel();
  REQUIRE(a.wait_for(30s) == WaitResult::finished);
  REQUIRE(b.wait_for(30s) == WaitResult::finished);
  CHECK(first.count(GenerationStatus::succeeded) == 40);
  INFO(first.first_wrong());
  CHECK(first.all_exact());  // the frames, not just the tally
  CHECK(second.count() == 5);
  CHECK(second.count(GenerationStatus::cancelled) == 5);
}

TEST_CASE("async: cancel_all covers every in-flight batch", "[async][cancel]") {
  auto g = open_counter();
  Collector col;
  std::latch first{1};
  std::atomic<bool> signalled{false};
  std::latch cancel_issued{1};
  AsyncRequest a = g.generate_images(range(0, 50), [&](Completion c) {
    col.add(std::move(c));
    if (!signalled.exchange(true)) {
      first.count_down();
      cancel_issued.wait();  // hold the worker in item 0 until cancel_all() has landed
    }
  });
  AsyncRequest b = g.generate_images(range(50, 100), col.handler());
  first.wait();
  g.cancel_all();
  cancel_issued.count_down();
  REQUIRE(a.wait_for(30s) == WaitResult::finished);
  REQUIRE(b.wait_for(30s) == WaitResult::finished);
  CHECK(col.count() == 100);
  CHECK(col.count(GenerationStatus::succeeded) == 1);
  INFO(col.first_wrong());
  CHECK(col.all_exact());  // the frames, not just the tally
  CHECK(col.count(GenerationStatus::cancelled) == 99);
  CHECK(col.count(GenerationStatus::failed) == 0);
}

TEST_CASE("async: destroying the generator mid-flight delivers cancelled for the rest",
          "[async][cancel]") {
  Collector col;
  std::latch first{1};
  std::atomic<bool> signalled{false};
  std::atomic<bool> destroying{false};
  AsyncRequest req;
  {
    auto g = open_counter();
    auto* gp = &g;
    req = g.generate_images(range(0, 100), [&, gp](Completion c) {
      col.add(std::move(c));
      if (!signalled.exchange(true)) first.count_down();
      // Hold the worker inside the first handler until teardown has actually begun, so exactly one
      // item can have completed. Waiting for the main thread to *call* the destructor is not the
      // same thing: the worker could start and finish the next item in between. A synchronous
      // request from the handler reports `cancelled` once the stop flag is set, which is precisely
      // the edge being waited for.
      while (!destroying.load() || gp->image_at(Time::zero()).has_value())
        std::this_thread::yield();
    });
    first.wait();
    destroying.store(true);
  }  // ~AssetImageGenerator joins the worker
  CHECK(req.finished());
  CHECK(col.count() == 100);
  CHECK(col.count(GenerationStatus::succeeded) == 1);
  INFO(col.first_wrong());
  CHECK(col.all_exact());  // the frames, not just the tally
  CHECK(col.count(GenerationStatus::cancelled) == 99);
  // The handle outlives the generator and stays safe to use.
  req.cancel();
  CHECK(req.wait() == WaitResult::finished);
  CHECK(req.wait_for(1ms) == WaitResult::finished);
  CHECK(req.completed() == 100);
}

// "Destroying the generator joins its worker" is the whole teardown contract, and the suite only
// ever checked its observable half — that the completions arrive. A generator that cancelled and
// delivered correctly but detached its worker would pass every other async test. Counting threads
// across repeated open/cancel/destroy cycles is what makes the join itself an assertion.
TEST_CASE("async: open/cancel/destroy cycles leak no threads", "[async][lifetime]") {
  if (!live_threads()) {
    SUCCEED("thread count unavailable on this platform");
    return;
  }
  constexpr int kCycles = 40;
  constexpr int kItems = 60;
  // One warm-up cycle before the baseline: a sanitizer runtime starts its own background thread on
  // first use, and counting that as our leak would be a false positive under TSan.
  { auto warm = open_counter(); }
  const std::optional<std::size_t> before = live_threads();
  REQUIRE(before.has_value());
  for (int i = 0; i < kCycles; ++i) {
    std::atomic<int> delivered{0};  // outlives `g`, so the handler's capture stays valid
    auto g = open_counter();
    auto req = g.generate_images(range(0, kItems), [&](Completion) { delivered.fetch_add(1); });
    req.cancel();
    g.close();  // cancels, delivers the rest, joins the worker
    INFO("cycle " << i);
    REQUIRE(delivered.load() == kItems);  // exactly-once holds through cancellation
    REQUIRE(req.finished());
  }
  const std::optional<std::size_t> after = live_threads();
  REQUIRE(after.has_value());
  INFO(kCycles << " cycles: " << *before << " threads before, " << *after << " after");
  CHECK(*after <= *before);
}

TEST_CASE("async: submissions from several threads", "[async]") {
  auto g = open_counter();
  Collector col;
  std::vector<AsyncRequest> reqs(8);
  {
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, t] {
        reqs[static_cast<std::size_t>(t) * 2] =
            g.generate_images(frames({t, 40 + t, 80 + t}), col.handler());
        reqs[static_cast<std::size_t>(t) * 2 + 1] =
            g.generate_images(frames({119 - t, 20 + t}), col.handler());
      });
    }
    for (auto& th : threads) th.join();
  }
  for (auto& r : reqs) REQUIRE(r.wait_for(30s) == WaitResult::finished);
  CHECK(col.count() == 20);
  CHECK(col.count(GenerationStatus::succeeded) == 20);
  INFO(col.first_wrong());
  CHECK(col.all_exact());  // the frames, not just the tally
  for (const auto& item : col.items()) CHECK(*item.index == frame_index_of_time(item.requested));
}

TEST_CASE("async: synchronous calls interleave with a running batch", "[async]") {
  auto g = open_counter();
  Collector col;
  std::latch first{1};
  std::atomic<bool> signalled{false};
  AsyncRequest req = g.generate_images(range(0, 120), [&](Completion c) {
    col.add(std::move(c));
    if (!signalled.exchange(true)) first.count_down();
  });
  first.wait();
  auto img = REQUIRE_OK(g.image_at(Time{77, 30}));
  CHECK(frame_index_of(img) == 77);
  CHECK(col.count() < 120);  // the sync call did not wait for the whole batch
  REQUIRE(req.wait_for(30s) == WaitResult::finished);
  CHECK(col.count(GenerationStatus::succeeded) == 120);
  INFO(col.first_wrong());
  CHECK(col.all_exact());  // the frames, not just the tally
  for (const auto& item : col.items()) CHECK(*item.index == frame_index_of_time(item.requested));
}

TEST_CASE("async: generator calls from inside a handler", "[async]") {
  auto g = open_counter();
  Collector inner;
  std::optional<int> sync_from_handler;
  WaitResult wait_result = WaitResult::finished;
  AsyncRequest req;
  AsyncRequest nested;
  std::latch handle_assigned{1};  // the handler reads `req`, so wait until the caller stored it
  std::latch done{1};
  req = g.generate_images({Time{10, 30}}, [&](Completion c) {
    handle_assigned.wait();
    auto s = g.image_at(Time{20, 30});  // allowed: the decoder mutex is not held here
    if (s) sync_from_handler = frame_index_of(*s);
    wait_result = req.wait_for(1s);  // must return immediately without deadlock
    nested = g.generate_images({Time{30, 30}}, inner.handler());  // allowed
    (void)c;
    done.count_down();
  });
  handle_assigned.count_down();
  done.wait();
  REQUIRE(req.wait_for(30s) == WaitResult::finished);
  CHECK(sync_from_handler == 20);
  CHECK(wait_result == WaitResult::refused_on_worker_thread);
  REQUIRE(nested.wait_for(30s) == WaitResult::finished);
  REQUIRE(inner.wait_for_count(1));
  CHECK(*inner.items()[0].index == 30);
}

TEST_CASE("async: waiting on a later batch from inside a handler is refused, not deadlocked",
          "[async]") {
  auto g = open_counter();
  Collector second;
  AsyncRequest later;
  std::latch assigned{1};
  std::latch done{1};
  WaitResult refused = WaitResult::finished;
  AsyncRequest first = g.generate_images({Time{1, 30}}, [&](Completion) {
    assigned.wait();
    refused = later.wait_for(30s);  // `later` is queued behind this batch: must return at once
    done.count_down();
  });
  later = g.generate_images({Time{2, 30}}, second.handler());
  assigned.count_down();
  done.wait();
  CHECK(refused == WaitResult::refused_on_worker_thread);
  REQUIRE(first.wait_for(30s) == WaitResult::finished);
  REQUIRE(later.wait_for(30s) == WaitResult::finished);
  CHECK(second.count(GenerationStatus::succeeded) == 1);
  INFO(second.first_wrong());
  CHECK(second.all_exact());  // the frames, not just the tally
}

TEST_CASE("async: destroying the generator from inside a handler", "[async][cancel]") {
  Collector col;
  std::latch all_delivered{10};
  auto holder = std::make_unique<AssetImageGenerator>(open_counter());
  AssetImageGenerator* raw = holder.get();
  AsyncRequest req = raw->generate_images(range(0, 10), [&](Completion c) {
    col.add(std::move(c));
    if (holder) holder.reset();  // teardown from the worker thread
    all_delivered.count_down();
  });
  all_delivered.wait();
  REQUIRE(req.wait_for(30s) == WaitResult::finished);
  CHECK(col.count() == 10);
  CHECK(col.count(GenerationStatus::succeeded) == 1);
  INFO(col.first_wrong());
  CHECK(col.all_exact());  // the frames, not just the tally
  CHECK(col.count(GenerationStatus::cancelled) == 9);
}

TEST_CASE("async: degenerate inputs", "[async]") {
  auto g = open_counter();
  SECTION("empty span finishes immediately without calling the handler") {
    std::atomic<int> calls{0};
    AsyncRequest req = g.generate_images(std::span<const Time>{}, [&](Completion) { ++calls; });
    CHECK(req.finished());
    CHECK(req.size() == 0);
    REQUIRE(req.wait_for(1ms) == WaitResult::finished);
    CHECK(calls == 0);
  }
  SECTION("default-constructed handle") {
    AsyncRequest none;
    CHECK_FALSE(none.valid());
    CHECK(none.finished());
    none.cancel();
    CHECK(none.wait() == WaitResult::finished);
    CHECK_FALSE(static_cast<bool>(none));
  }
  SECTION("move-only handler state compiles and runs") {
    auto token = std::make_unique<int>(7);
    Collector col;
    AsyncRequest req =
        g.generate_images({Time{1, 30}}, [&col, tok = std::move(token)](Completion c) {
          REQUIRE(*tok == 7);
          col.add(std::move(c));
        });
    REQUIRE(req.wait_for(30s) == WaitResult::finished);
    CHECK(col.count(GenerationStatus::succeeded) == 1);
    INFO(col.first_wrong());
    CHECK(col.all_exact());  // the frames, not just the tally
  }
  SECTION("moved-from generator") {
    AssetImageGenerator moved = std::move(g);
    CHECK_FALSE(g.is_open());  // NOLINT(bugprone-use-after-move)
    REQUIRE_ERROR(g.image_at(Time::zero()), stills::ErrorCode::invalid_state);
    g.cancel_all();
    CHECK(moved.is_open());
    auto img = REQUIRE_OK(moved.image_at(Time{3, 30}));
    CHECK(frame_index_of(img) == 3);
  }
}

// A completion handler is documented as free to call generate_images(), and destruction is
// documented to deliver `cancelled` for every undelivered item — so the handler runs *during*
// ~AssetImageGenerator and chains onto a generator whose teardown has begun. The chained batch is
// queued and the drain delivers it `cancelled`, which is the signal a chaining handler needs to
// stop; anything else leaves it unable to tell teardown from an ordinary cancel().
TEST_CASE("async: a handler may chain a request while the generator is destroyed",
          "[async][teardown]") {
  std::atomic<int> chained{0};
  std::atomic<int> chained_completions{0};
  for (int iteration = 0; iteration < 40; ++iteration) {
    std::vector<Time> times;
    for (int i = 0; i < 40; ++i) times.push_back(Time{i * 3, 30});
    std::optional<stills::AssetImageGenerator> g{open_counter()};
    auto* gp = &*g;
    auto req = gp->generate_images(times, [&, gp](stills::Completion) {
      ++chained;
      (void)gp->generate_images({Time{1, 30}}, [&](stills::Completion) { ++chained_completions; });
    });
    // Destroy part-way through, so the drain runs the handler while `stopping` is set.
    std::this_thread::sleep_for(std::chrono::microseconds{200 + (iteration % 7) * 300});
    g.reset();
    CHECK(req.finished());
  }
  INFO("chained " << chained.load() << " batches, " << chained_completions.load()
                  << " of their items delivered");
  CHECK(chained.load() > 0);
}

// A handler cannot name its own AsyncRequest — it exists before generate_images() has returned the
// handle, and hoisting a handle for the handler to capture is a data race between the caller
// writing it and the worker reading it. Completion::cancel_batch() is the way to stop a batch from
// inside its own handler ("cancel after N results").
TEST_CASE("async: a handler cancels its own batch with Completion::cancel_batch",
          "[async][cancel]") {
  auto g = open_counter();
  std::atomic<int> succeeded{0};
  Collector col;
  std::vector<Time> times;
  for (int n = 0; n < 60; ++n) times.emplace_back(n * 2, 30);
  auto req = g.generate_images(times, [&](Completion c) {
    if (c.status() == GenerationStatus::succeeded && ++succeeded == 5) c.cancel_batch();
    col.add(std::move(c));
  });
  REQUIRE(req.wait() == WaitResult::finished);
  const auto items = col.items();
  CHECK(items.size() == times.size());  // exactly one completion per time, cancelled or not
  CHECK(succeeded.load() >= 5);
  CHECK(col.count(GenerationStatus::cancelled) > 0);
}

// A Completion owns its Image, so moving it out of the handler is the only way to keep the frame —
// and a Completion kept that way outlives the batch it came from. cancel_batch() on it must then be
// a no-op, not a write through a dangling pointer (it was one, until the batch reference
// became weak).
TEST_CASE("async: cancel_batch on a Completion outliving its batch is a no-op",
          "[async][cancel][lifetime]") {
  std::optional<Completion> saved;
  {
    auto g = open_counter();
    auto req = g.generate_images(frames({0, 30}), [&](Completion c) {
      if (c.index == 0) saved.emplace(std::move(c));
    });
    REQUIRE(req.wait_for(30s) == WaitResult::finished);
  }  // worker joined and the last handle dropped: the batch is destroyed here
  REQUIRE(saved.has_value());
  REQUIRE(saved->status() == GenerationStatus::succeeded);
  saved->cancel_batch();  // must not touch the freed batch
  saved->cancel_batch();  // still idempotent
  CHECK(frame_index_of(*saved->result) == 0);
}

// AsyncRequest is copyable and copies observe one batch, so cancelling through any of them cancels
// it; and completed() lags the handler by one, because it counts deliveries that have returned.
TEST_CASE("async: AsyncRequest copies share cancellation", "[async][cancel]") {
  auto g = open_counter();
  Collector col;
  std::vector<Time> times;
  for (int n = 0; n < 40; ++n) times.emplace_back(n * 3, 30);
  auto req = g.generate_images(times, col.handler());
  AsyncRequest copy = req;  // NOLINT(performance-unnecessary-copy-initialization)
  CHECK(copy.size() == req.size());
  copy.cancel();
  REQUIRE(req.wait() == WaitResult::finished);
  CHECK(req.finished());
  CHECK(col.count() == times.size());
  CHECK(col.count(GenerationStatus::cancelled) > 0);
}

// std::mutex is not fair, so without Engine::sync_waiters a synchronous image_at() issued while a
// long batch is running has no bound on how long it waits: the worker would reacquire the decoder
// between items ahead of it every time. Both modes on one generator is the brief's core ask, so
// this is the test of that handoff.
TEST_CASE("async: a waiting sync caller is served before the next async item",
          "[async][sync]") {
  auto g = open_counter();
  std::vector<Time> times;
  for (int n = 0; n < 120; ++n) times.emplace_back(n, 30);
  std::atomic<int> delivered{0};
  std::latch started{1};
  auto req = g.generate_images(times, [&](Completion) {
    if (delivered.fetch_add(1) == 0) started.count_down();
  });
  started.wait();
  const auto t0 = std::chrono::steady_clock::now();
  auto img = REQUIRE_OK(g.image_at(Time{100, 30}));
  const auto waited = std::chrono::steady_clock::now() - t0;
  CHECK(frame_index_of(img) == 100);
  CHECK(delivered.load() < 120);  // did not wait for the batch
  CHECK(waited < 2s);
  REQUIRE(req.wait_for(30s) == stills::WaitResult::finished);
}

// The other side of that handoff: the sync-first policy has to be bounded, or it trades one
// unbounded wait for its opposite. Four threads calling image_at() in a loop never leave
// `sync_waiters` at zero for the worker to observe, so an unbounded policy dispatches next to
// nothing. The bound is Engine::sync_priority_limit; the deadline here is far longer than the
// twenty items need so that a loaded machine does not fail it, but far shorter than starvation.
TEST_CASE("async: synchronous callers cannot starve a running batch",
          "[async][sync][starvation]") {
  auto g = open_counter();
  std::vector<Time> times = range(0, 20);
  std::atomic<int> delivered{0};
  auto req = g.generate_images(times, [&](Completion) { delivered.fetch_add(1); });

  std::atomic<bool> stop{false};
  std::atomic<long> sync_calls{0};
  std::vector<std::thread> hogs;
  for (int i = 0; i < 4; ++i) {
    hogs.emplace_back([&, i] {
      int n = i * 7;
      while (!stop.load(std::memory_order_relaxed)) {
        (void)g.image_at(Time{n % 120, 30});
        n += 11;
        sync_calls.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  const auto t0 = std::chrono::steady_clock::now();
  const WaitResult r = req.wait_for(10s);  // the bound puts this at well under a second
  const auto waited = std::chrono::steady_clock::now() - t0;
  stop.store(true);
  for (std::thread& h : hogs) h.join();

  INFO("delivered " << delivered.load() << "/" << times.size() << " in "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()
                    << " ms while " << sync_calls.load() << " sync calls ran");
  CHECK(r == WaitResult::finished);
  CHECK(delivered.load() == static_cast<int>(times.size()));
}

// The bound above and the documented "a handler may call straight back in" have to hold at the
// same time. The worker claims its turn from the same thread the handler then runs on, so a turn
// held across the handler would park that handler's own image_at() on a flag only it could clear.
TEST_CASE("async: a handler may call image_at() while synchronous callers are saturating",
          "[async][sync][starvation]") {
  auto g = open_counter();
  std::vector<Time> times = range(0, 8);
  std::atomic<int> inner_ok{0};
  auto req = g.generate_images(times, [&](Completion) {
    if (g.image_at(Time{15, 30})) inner_ok.fetch_add(1);  // on the worker thread
  });

  std::atomic<bool> stop{false};
  std::vector<std::thread> hogs;
  for (int i = 0; i < 4; ++i) {
    hogs.emplace_back([&, i] {
      int n = i * 7;
      while (!stop.load(std::memory_order_relaxed)) {
        (void)g.image_at(Time{n % 120, 30});
        n += 11;
      }
    });
  }
  const WaitResult r = req.wait_for(20s);
  stop.store(true);
  for (std::thread& h : hogs) h.join();
  CHECK(r == WaitResult::finished);  // a deadlock here is the whole point of the test
  CHECK(inner_ok.load() == static_cast<int>(times.size()));
}

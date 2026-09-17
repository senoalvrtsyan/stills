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

namespace
{
std::vector<Time> frames (std::initializer_list<int> ns)
{
    std::vector<Time> v;

    for (int n : ns)
        v.emplace_back (n, 30);
    return v;
}

std::vector<Time> range (int from, int to)
{
    std::vector<Time> v;

    for (int n = from; n < to; ++n)
        v.emplace_back (n, 30);
    return v;
}

/// Live threads in this process, or nullopt where that cannot be asked. /proc/self/task is the
/// only portable-enough answer; there is no equivalent on macOS without <mach/…>, so the test that
/// uses this reports itself satisfied rather than failing on a platform it cannot measure.
std::optional<std::size_t> liveThreads()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path task{ "/proc/self/task" };

    if (! fs::is_directory (task, ec) || ec) return std::nullopt;
    std::size_t n = 0;

    for (fs::directory_iterator it (task, ec); ! ec && it != fs::directory_iterator(); it.increment (ec))
        ++n;

    if (ec) return std::nullopt;
    return n;
}
} // namespace

TEST_CASE ("async: exactly-once, in-order delivery on the worker thread", "[async]")
{
    auto g = openCounter();
    Collector col;
    const auto times = frames ({ 5, 90, 3, 31, 119, 0, 60, 60, 29, 45 });
    AsyncRequest req = g.generateImages (times, col.handler());
    REQUIRE (req.isValid());
    CHECK (req.getSize() == times.size());
    REQUIRE (req.waitFor (30s) == WaitResult::finished);
    CHECK (req.isFinished());
    CHECK (req.getCompleted() == times.size());
    const auto items = col.getItems();
    REQUIRE (items.size() == times.size());

    for (std::size_t i = 0; i < items.size(); ++i)
    {
        CHECK (items[i].position == i);
        CHECK (items[i].requested == times[i]);
        CHECK (items[i].status == GenerationStatus::succeeded);
        REQUIRE (items[i].index.has_value());
        CHECK (*items[i].index == frameIndexOfTime (times[i]));
        CHECK (items[i].actual == times[i]);
        CHECK (items[i].thread != std::this_thread::get_id());
    }
}

TEST_CASE ("async: failures are delivered as failed, not dropped", "[async]")
{
    auto g = openCounter();
    Collector col;
    AsyncRequest req = g.generateImages ({ Time{ 1, 30 }, Time{ 99, 1 }, Time{ -1, 1 }, Time{ 2, 30 } }, col.handler());
    REQUIRE (req.waitFor (30s) == WaitResult::finished);
    const auto items = col.getItems();
    REQUIRE (items.size() == 4);
    CHECK (items[0].status == GenerationStatus::succeeded);
    CHECK (items[1].status == GenerationStatus::failed);
    CHECK (items[1].error == stills::ErrorCode::timeOutOfRange);
    CHECK (items[2].status == GenerationStatus::failed);
    CHECK (items[2].error == stills::ErrorCode::invalidArgument);
    CHECK (items[3].status == GenerationStatus::succeeded);
}

TEST_CASE ("async: cancel after some deliveries", "[async][cancel]")
{
    auto g = openCounter();
    Collector col;
    std::latch thirdDelivered{ 1 };
    std::latch cancelIssued{ 1 };
    std::atomic<int> delivered{ 0 };
    const auto times = range (0, 60);
    AsyncRequest req =
        g.generateImages (times,
                          [&] (Completion c)
                          {
                              col.add (std::move (c));

                              if (++delivered == 3)
                              {
                                  thirdDelivered.count_down();
                                  cancelIssued.wait(); // hold the worker inside this handler until cancel() has landed
                              }
                          });

    thirdDelivered.wait();
    req.cancel();
    req.cancel(); // idempotent
    cancelIssued.count_down();
    REQUIRE (req.waitFor (30s) == WaitResult::finished);
    const auto items = col.getItems();
    REQUIRE (items.size() == times.size());
    CHECK (col.count (GenerationStatus::failed) == 0);
    CHECK (col.count (GenerationStatus::succeeded) == 3); // deterministic: cancel landed before item 4 started
    INFO (col.firstWrong());
    CHECK (col.allExact()); // the frames, not just the tally
    CHECK (col.count (GenerationStatus::cancelled) == times.size() - 3);

    for (std::size_t i = 0; i < 3; ++i)
        CHECK (items[i].status == GenerationStatus::succeeded);

    for (std::size_t i = 3; i < items.size(); ++i)
        CHECK (items[i].status == GenerationStatus::cancelled);

    for (std::size_t i = 0; i < items.size(); ++i)
        CHECK (items[i].requested == times[i]);
}

TEST_CASE ("async: a queued batch cancelled before it starts is fully cancelled", "[async][cancel]")
{
    auto g = openCounter();
    Collector first;
    Collector second;
    AsyncRequest a = g.generateImages (range (0, 40), first.handler());
    AsyncRequest b = g.generateImages (range (0, 5), second.handler());
    b.cancel();
    REQUIRE (a.waitFor (30s) == WaitResult::finished);
    REQUIRE (b.waitFor (30s) == WaitResult::finished);
    CHECK (first.count (GenerationStatus::succeeded) == 40);
    INFO (first.firstWrong());
    CHECK (first.allExact()); // the frames, not just the tally
    CHECK (second.count() == 5);
    CHECK (second.count (GenerationStatus::cancelled) == 5);
}

TEST_CASE ("async: cancelAll covers every in-flight batch", "[async][cancel]")
{
    auto g = openCounter();
    Collector col;
    std::latch first{ 1 };
    std::atomic<bool> signalled{ false };
    std::latch cancelIssued{ 1 };
    AsyncRequest a =
        g.generateImages (range (0, 50),
                          [&] (Completion c)
                          {
                              col.add (std::move (c));

                              if (! signalled.exchange (true))
                              {
                                  first.count_down();
                                  cancelIssued.wait(); // hold the worker in item 0 until cancelAll() has landed
                              }
                          });

    AsyncRequest b = g.generateImages (range (50, 100), col.handler());
    first.wait();
    g.cancelAll();
    cancelIssued.count_down();
    REQUIRE (a.waitFor (30s) == WaitResult::finished);
    REQUIRE (b.waitFor (30s) == WaitResult::finished);
    CHECK (col.count() == 100);
    CHECK (col.count (GenerationStatus::succeeded) == 1);
    INFO (col.firstWrong());
    CHECK (col.allExact()); // the frames, not just the tally
    CHECK (col.count (GenerationStatus::cancelled) == 99);
    CHECK (col.count (GenerationStatus::failed) == 0);
}

TEST_CASE ("async: destroying the generator mid-flight delivers cancelled for the rest", "[async][cancel]")
{
    Collector col;
    std::latch first{ 1 };
    std::atomic<bool> signalled{ false };
    std::atomic<bool> destroying{ false };
    AsyncRequest req;
    {
        auto g = openCounter();
        auto* gp = &g;
        req = g.generateImages (range (0, 100),
                                [&, gp] (Completion c)
                                {
                                    col.add (std::move (c));

                                    if (! signalled.exchange (true)) first.count_down();
                                    // Hold the worker inside the first handler until teardown has actually begun, so
                                    // exactly one item can have completed. Waiting for the main thread to *call* the
                                    // destructor is not the same thing: the worker could start and finish the next
                                    // item in between. A synchronous request from the handler reports `cancelled` once
                                    // the stop flag is set, which is precisely the edge being waited for.
                                    while (! destroying.load() || gp->imageAt (Time::zero()).has_value())
                                        std::this_thread::yield();
                                });

        first.wait();
        destroying.store (true);
    } // ~AssetImageGenerator joins the worker

    CHECK (req.isFinished());
    CHECK (col.count() == 100);
    CHECK (col.count (GenerationStatus::succeeded) == 1);
    INFO (col.firstWrong());
    CHECK (col.allExact()); // the frames, not just the tally
    CHECK (col.count (GenerationStatus::cancelled) == 99);
    // The handle outlives the generator and stays safe to use.
    req.cancel();
    CHECK (req.wait() == WaitResult::finished);
    CHECK (req.waitFor (1ms) == WaitResult::finished);
    CHECK (req.getCompleted() == 100);
}

// "Destroying the generator joins its worker" is the whole teardown contract, and the suite only
// ever checked its observable half — that the completions arrive. A generator that cancelled and
// delivered correctly but detached its worker would pass every other async test. Counting threads
// across repeated open/cancel/destroy cycles is what makes the join itself an assertion.
TEST_CASE ("async: open/cancel/destroy cycles leak no threads", "[async][lifetime]")
{
    if (! liveThreads())
    {
        SUCCEED ("thread count unavailable on this platform");
        return;
    }

    constexpr int kCycles = 40;
    constexpr int kItems = 60;
    // One warm-up cycle before the baseline: a sanitizer runtime starts its own background thread on
    // first use, and counting that as our leak would be a false positive under TSan.
    {
        auto warm = openCounter();
    }

    const std::optional<std::size_t> before = liveThreads();
    REQUIRE (before.has_value());

    for (int i = 0; i < kCycles; ++i)
    {
        std::atomic<int> delivered{ 0 }; // outlives `g`, so the handler's capture stays valid
        auto g = openCounter();
        auto req = g.generateImages (range (0, kItems), [&] (Completion) { delivered.fetch_add (1); });
        req.cancel();
        g.close(); // cancels, delivers the rest, joins the worker
        INFO ("cycle " << i);
        REQUIRE (delivered.load() == kItems); // exactly-once holds through cancellation
        REQUIRE (req.isFinished());
    }

    const std::optional<std::size_t> after = liveThreads();
    REQUIRE (after.has_value());
    INFO (kCycles << " cycles: " << *before << " threads before, " << *after << " after");
    CHECK (*after <= *before);
}

TEST_CASE ("async: submissions from several threads", "[async]")
{
    auto g = openCounter();
    Collector col;
    std::vector<AsyncRequest> reqs (8);
    {
        std::vector<std::thread> threads;

        for (int t = 0; t < 4; ++t)
        {
            threads.emplace_back (
                [&, t]
                {
                    reqs[static_cast<std::size_t> (t) * 2] =
                        g.generateImages (frames ({ t, 40 + t, 80 + t }), col.handler());
                    reqs[static_cast<std::size_t> (t) * 2 + 1] =
                        g.generateImages (frames ({ 119 - t, 20 + t }), col.handler());
                });
        }

        for (auto& th : threads)
            th.join();
    }

    for (auto& r : reqs)
        REQUIRE (r.waitFor (30s) == WaitResult::finished);
    CHECK (col.count() == 20);
    CHECK (col.count (GenerationStatus::succeeded) == 20);
    INFO (col.firstWrong());
    CHECK (col.allExact()); // the frames, not just the tally

    for (const auto& item : col.getItems())
        CHECK (*item.index == frameIndexOfTime (item.requested));
}

TEST_CASE ("async: synchronous calls interleave with a running batch", "[async]")
{
    auto g = openCounter();
    Collector col;
    std::latch first{ 1 };
    std::atomic<bool> signalled{ false };
    AsyncRequest req = g.generateImages (range (0, 120),
                                         [&] (Completion c)
                                         {
                                             col.add (std::move (c));

                                             if (! signalled.exchange (true)) first.count_down();
                                         });

    first.wait();
    auto img = REQUIRE_OK (g.imageAt (Time{ 77, 30 }));
    CHECK (frameIndexOf (img) == 77);
    CHECK (col.count() < 120); // the sync call did not wait for the whole batch
    REQUIRE (req.waitFor (30s) == WaitResult::finished);
    CHECK (col.count (GenerationStatus::succeeded) == 120);
    INFO (col.firstWrong());
    CHECK (col.allExact()); // the frames, not just the tally

    for (const auto& item : col.getItems())
        CHECK (*item.index == frameIndexOfTime (item.requested));
}

TEST_CASE ("async: generator calls from inside a handler", "[async]")
{
    auto g = openCounter();
    Collector inner;
    std::optional<int> syncFromHandler;
    WaitResult waitResult = WaitResult::finished;
    AsyncRequest req;
    AsyncRequest nested;
    std::latch handleAssigned{ 1 }; // the handler reads `req`, so wait until the caller stored it
    std::latch done{ 1 };
    req = g.generateImages ({ Time{ 10, 30 } },
                            [&] (Completion c)
                            {
                                handleAssigned.wait();
                                auto s = g.imageAt (Time{ 20, 30 }); // allowed: the decoder mutex is not held here

                                if (s) syncFromHandler = frameIndexOf (*s);
                                waitResult = req.waitFor (1s); // must return immediately without deadlock
                                nested = g.generateImages ({ Time{ 30, 30 } }, inner.handler()); // allowed
                                (void)c;
                                done.count_down();
                            });

    handleAssigned.count_down();
    done.wait();
    REQUIRE (req.waitFor (30s) == WaitResult::finished);
    CHECK (syncFromHandler == 20);
    CHECK (waitResult == WaitResult::refusedOnWorkerThread);
    REQUIRE (nested.waitFor (30s) == WaitResult::finished);
    REQUIRE (inner.waitForCount (1));
    CHECK (*inner.getItems()[0].index == 30);
}

TEST_CASE ("async: waiting on a later batch from inside a handler is refused, not deadlocked", "[async]")
{
    auto g = openCounter();
    Collector second;
    AsyncRequest later;
    std::latch assigned{ 1 };
    std::latch done{ 1 };
    WaitResult refused = WaitResult::finished;
    AsyncRequest first =
        g.generateImages ({ Time{ 1, 30 } },
                          [&] (Completion)
                          {
                              assigned.wait();
                              refused = later.waitFor (30s); // `later` is queued behind this batch: must return at once
                              done.count_down();
                          });

    later = g.generateImages ({ Time{ 2, 30 } }, second.handler());
    assigned.count_down();
    done.wait();
    CHECK (refused == WaitResult::refusedOnWorkerThread);
    REQUIRE (first.waitFor (30s) == WaitResult::finished);
    REQUIRE (later.waitFor (30s) == WaitResult::finished);
    CHECK (second.count (GenerationStatus::succeeded) == 1);
    INFO (second.firstWrong());
    CHECK (second.allExact()); // the frames, not just the tally
}

TEST_CASE ("async: destroying the generator from inside a handler", "[async][cancel]")
{
    Collector col;
    std::latch allDelivered{ 10 };
    auto holder = std::make_unique<AssetImageGenerator> (openCounter());
    AssetImageGenerator* raw = holder.get();
    AsyncRequest req = raw->generateImages (range (0, 10),
                                            [&] (Completion c)
                                            {
                                                col.add (std::move (c));

                                                if (holder) holder.reset(); // teardown from the worker thread
                                                allDelivered.count_down();
                                            });

    allDelivered.wait();
    REQUIRE (req.waitFor (30s) == WaitResult::finished);
    CHECK (col.count() == 10);
    CHECK (col.count (GenerationStatus::succeeded) == 1);
    INFO (col.firstWrong());
    CHECK (col.allExact()); // the frames, not just the tally
    CHECK (col.count (GenerationStatus::cancelled) == 9);
}

TEST_CASE ("async: degenerate inputs", "[async]")
{
    auto g = openCounter();
    SECTION ("empty span finishes immediately without calling the handler")
    {
        std::atomic<int> calls{ 0 };
        AsyncRequest req = g.generateImages (std::span<const Time>{}, [&] (Completion) { ++calls; });
        CHECK (req.isFinished());
        CHECK (req.getSize() == 0);
        REQUIRE (req.waitFor (1ms) == WaitResult::finished);
        CHECK (calls == 0);
    }

    SECTION ("default-constructed handle")
    {
        AsyncRequest none;
        CHECK_FALSE (none.isValid());
        CHECK (none.isFinished());
        none.cancel();
        CHECK (none.wait() == WaitResult::finished);
        CHECK_FALSE (static_cast<bool> (none));
    }

    SECTION ("move-only handler state compiles and runs")
    {
        auto token = std::make_unique<int> (7);
        Collector col;
        AsyncRequest req = g.generateImages ({ Time{ 1, 30 } },
                                             [&col, tok = std::move (token)] (Completion c)
                                             {
                                                 REQUIRE (*tok == 7);
                                                 col.add (std::move (c));
                                             });

        REQUIRE (req.waitFor (30s) == WaitResult::finished);
        CHECK (col.count (GenerationStatus::succeeded) == 1);
        INFO (col.firstWrong());
        CHECK (col.allExact()); // the frames, not just the tally
    }

    SECTION ("moved-from generator")
    {
        AssetImageGenerator moved = std::move (g);
        CHECK_FALSE (g.isOpen()); // NOLINT(bugprone-use-after-move)
        REQUIRE_ERROR (g.imageAt (Time::zero()), stills::ErrorCode::invalidState);
        g.cancelAll();
        CHECK (moved.isOpen());
        auto img = REQUIRE_OK (moved.imageAt (Time{ 3, 30 }));
        CHECK (frameIndexOf (img) == 3);
    }
}

// A completion handler is documented as free to call generateImages(), and destruction is
// documented to deliver `cancelled` for every undelivered item — so the handler runs *during*
// ~AssetImageGenerator and chains onto a generator whose teardown has begun. The chained batch is
// queued and the drain delivers it `cancelled`, which is the signal a chaining handler needs to
// stop; anything else leaves it unable to tell teardown from an ordinary cancel().
TEST_CASE ("async: a handler may chain a request while the generator is destroyed", "[async][teardown]")
{
    std::atomic<int> chained{ 0 };
    std::atomic<int> chainedCompletions{ 0 };

    for (int iteration = 0; iteration < 40; ++iteration)
    {
        std::vector<Time> times;

        for (int i = 0; i < 40; ++i)
            times.push_back (Time{ i * 3, 30 });
        std::optional<stills::AssetImageGenerator> g{ openCounter() };
        auto* gp = &*g;
        auto req = gp->generateImages (times,
                                       [&, gp] (stills::Completion)
                                       {
                                           ++chained;
                                           (void)gp->generateImages ({ Time{ 1, 30 } }, [&] (stills::Completion)
                                                                     { ++chainedCompletions; });
                                       });

        // Destroy part-way through, so the drain runs the handler while `stopping` is set.
        std::this_thread::sleep_for (std::chrono::microseconds{ 200 + (iteration % 7) * 300 });
        g.reset();
        CHECK (req.isFinished());
    }

    INFO ("chained " << chained.load() << " batches, " << chainedCompletions.load() << " of their items delivered");
    CHECK (chained.load() > 0);
}

// A handler cannot name its own AsyncRequest — it exists before generateImages() has returned the
// handle, and hoisting a handle for the handler to capture is a data race between the caller
// writing it and the worker reading it. Completion::cancelBatch() is the way to stop a batch from
// inside its own handler ("cancel after N results").
TEST_CASE ("async: a handler cancels its own batch with Completion::cancelBatch", "[async][cancel]")
{
    auto g = openCounter();
    std::atomic<int> succeeded{ 0 };
    Collector col;
    std::vector<Time> times;

    for (int n = 0; n < 60; ++n)
        times.emplace_back (n * 2, 30);
    auto req = g.generateImages (times,
                                 [&] (Completion c)
                                 {
                                     if (c.getStatus() == GenerationStatus::succeeded && ++succeeded == 5)
                                         c.cancelBatch();
                                     col.add (std::move (c));
                                 });

    REQUIRE (req.wait() == WaitResult::finished);
    const auto items = col.getItems();
    CHECK (items.size() == times.size()); // exactly one completion per time, cancelled or not
    CHECK (succeeded.load() >= 5);
    CHECK (col.count (GenerationStatus::cancelled) > 0);
}

// A Completion owns its Image, so moving it out of the handler is the only way to keep the frame —
// and a Completion kept that way outlives the batch it came from. cancelBatch() on it must then be
// a no-op, not a write through a dangling pointer (it was one, until the batch reference
// became weak).
TEST_CASE ("async: cancelBatch on a Completion outliving its batch is a no-op", "[async][cancel][lifetime]")
{
    std::optional<Completion> saved;
    {
        auto g = openCounter();
        auto req = g.generateImages (frames ({ 0, 30 }),
                                     [&] (Completion c)
                                     {
                                         if (c.index == 0) saved.emplace (std::move (c));
                                     });

        REQUIRE (req.waitFor (30s) == WaitResult::finished);
    } // worker joined and the last handle dropped: the batch is destroyed here

    REQUIRE (saved.has_value());
    REQUIRE (saved->getStatus() == GenerationStatus::succeeded);
    saved->cancelBatch(); // must not touch the freed batch
    saved->cancelBatch(); // still idempotent
    CHECK (frameIndexOf (*saved->result) == 0);
}

// AsyncRequest is copyable and copies observe one batch, so cancelling through any of them cancels
// it; and getCompleted() lags the handler by one, because it counts deliveries that have returned.
TEST_CASE ("async: AsyncRequest copies share cancellation", "[async][cancel]")
{
    auto g = openCounter();
    Collector col;
    std::vector<Time> times;

    for (int n = 0; n < 40; ++n)
        times.emplace_back (n * 3, 30);
    auto req = g.generateImages (times, col.handler());
    AsyncRequest copy = req; // NOLINT(performance-unnecessary-copy-initialization)
    CHECK (copy.getSize() == req.getSize());
    copy.cancel();
    REQUIRE (req.wait() == WaitResult::finished);
    CHECK (req.isFinished());
    CHECK (col.count() == times.size());
    CHECK (col.count (GenerationStatus::cancelled) > 0);
}

// std::mutex is not fair, so without Engine::syncWaiters a synchronous imageAt() issued while a
// long batch is running has no bound on how long it waits: the worker would reacquire the decoder
// between items ahead of it every time. Both modes on one generator is the brief's core ask, so
// this is the test of that handoff.
TEST_CASE ("async: a waiting sync caller is served before the next async item", "[async][sync]")
{
    auto g = openCounter();
    std::vector<Time> times;

    for (int n = 0; n < 120; ++n)
        times.emplace_back (n, 30);
    std::atomic<int> delivered{ 0 };
    std::latch started{ 1 };
    auto req = g.generateImages (times,
                                 [&] (Completion)
                                 {
                                     if (delivered.fetch_add (1) == 0) started.count_down();
                                 });

    started.wait();
    const auto t0 = std::chrono::steady_clock::now();
    auto img = REQUIRE_OK (g.imageAt (Time{ 100, 30 }));
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK (frameIndexOf (img) == 100);
    CHECK (delivered.load() < 120); // did not wait for the batch
    CHECK (waited < 2s);
    REQUIRE (req.waitFor (30s) == stills::WaitResult::finished);
}

// The other side of that handoff: the sync-first policy has to be bounded, or it trades one
// unbounded wait for its opposite. Four threads calling imageAt() in a loop never leave
// `syncWaiters` at zero for the worker to observe, so an unbounded policy dispatches next to
// nothing. The bound is Engine::syncPriorityLimit; the deadline here is far longer than the
// twenty items need so that a loaded machine does not fail it, but far shorter than starvation.
TEST_CASE ("async: synchronous callers cannot starve a running batch", "[async][sync][starvation]")
{
    auto g = openCounter();
    std::vector<Time> times = range (0, 20);
    std::atomic<int> delivered{ 0 };
    auto req = g.generateImages (times, [&] (Completion) { delivered.fetch_add (1); });

    std::atomic<bool> stop{ false };
    std::atomic<long> syncCalls{ 0 };
    std::vector<std::thread> hogs;

    for (int i = 0; i < 4; ++i)
    {
        hogs.emplace_back (
            [&, i]
            {
                int n = i * 7;

                while (! stop.load (std::memory_order_relaxed))
                {
                    (void)g.imageAt (Time{ n % 120, 30 });
                    n += 11;
                    syncCalls.fetch_add (1, std::memory_order_relaxed);
                }
            });
    }

    const auto t0 = std::chrono::steady_clock::now();
    const WaitResult r = req.waitFor (10s); // the bound puts this at well under a second
    const auto waited = std::chrono::steady_clock::now() - t0;
    stop.store (true);

    for (std::thread& h : hogs)
        h.join();

    INFO ("delivered " << delivered.load() << "/" << times.size() << " in "
                       << std::chrono::duration_cast<std::chrono::milliseconds> (waited).count() << " ms while "
                       << syncCalls.load() << " sync calls ran");
    CHECK (r == WaitResult::finished);
    CHECK (delivered.load() == static_cast<int> (times.size()));
}

// The bound above and the documented "a handler may call straight back in" have to hold at the
// same time. The worker claims its turn from the same thread the handler then runs on, so a turn
// held across the handler would park that handler's own imageAt() on a flag only it could clear.
TEST_CASE ("async: a handler may call imageAt() while synchronous callers are saturating", "[async][sync][starvation]")
{
    auto g = openCounter();
    std::vector<Time> times = range (0, 8);
    std::atomic<int> innerOk{ 0 };
    auto req = g.generateImages (times,
                                 [&] (Completion)
                                 {
                                     if (g.imageAt (Time{ 15, 30 })) innerOk.fetch_add (1); // on the worker thread
                                 });

    std::atomic<bool> stop{ false };
    std::vector<std::thread> hogs;

    for (int i = 0; i < 4; ++i)
    {
        hogs.emplace_back (
            [&, i]
            {
                int n = i * 7;

                while (! stop.load (std::memory_order_relaxed))
                {
                    (void)g.imageAt (Time{ n % 120, 30 });
                    n += 11;
                }
            });
    }

    const WaitResult r = req.waitFor (20s);
    stop.store (true);

    for (std::thread& h : hogs)
        h.join();
    CHECK (r == WaitResult::finished); // a deadlock here is the whole point of the test
    CHECK (innerOk.load() == static_cast<int> (times.size()));
}

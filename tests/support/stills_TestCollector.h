#pragma once
// Thread-safe collector for asynchronous completions. Never sleeps; every wait has a timeout so a
// hang fails the test with a message instead of stalling CTest.
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stills/stills_AssetImageGenerator.h>
#include <string>
#include <thread>
#include <vector>

#include "support/stills_TestFrameIndex.h"

namespace testsupport
{

struct Collected
{
    std::size_t position; // item index within the batch
    /// Completion::index again, under the name the per-item assertions below use.
    std::size_t sequence;
    stills::Time requested;
    stills::GenerationStatus status;
    std::optional<int> index; // frame index when an image came back
    std::optional<stills::Time> actual;
    std::optional<stills::ErrorCode> error;
    std::thread::id thread;

    /// The item with this sequence number, or null.
    [[nodiscard]] static const Collected* bySequence (const std::vector<Collected>& items, std::size_t seq)
    {
        for (const auto& i : items)
        {
            if (i.sequence == seq) return &i;
        }

        return nullptr;
    }
};

class Collector
{
public:
    /// Handler to pass to generateImages(). Captures `this`; the Collector must outlive the batch.
    auto handler (int rotation = 0)
    {
        return [this, rotation] (stills::Completion c) { add (std::move (c), rotation); };
    }

    void add (stills::Completion c, int rotation = 0)
    {
        Collected item{ c.index,      c.index,      c.requestedTime, c.getStatus(),
                        std::nullopt, std::nullopt, std::nullopt,    std::this_thread::get_id() };

        if (c.result)
        {
            item.index = frameIndexOf (*c.result, rotation);
            item.actual = c.result->getActualTime();
        }
        else
        {
            item.error = c.result.error().code;
        }

        // Notify while holding the lock: a waiter may destroy this Collector as soon as it wakes.
        std::lock_guard lk (mutex);
        items.push_back (std::move (item));
        cv.notify_all();
    }

    [[nodiscard]] bool waitForCount (std::size_t n, std::chrono::seconds timeout = std::chrono::seconds{ 30 })
    {
        std::unique_lock lk (mutex);
        return cv.wait_for (lk, timeout, [&] { return items.size() >= n; });
    }

    /// Blocks until `pred(collected)` holds or the timeout elapses; false on the timeout, so a test that
    /// is never satisfied fails with its own message instead of stalling CTest. `pred` runs under
    /// the collector's lock, so it must not call back into the collector.
    template <class Pred>
    [[nodiscard]] bool waitUntil (Pred pred, std::chrono::seconds timeout = std::chrono::seconds{ 30 })
    {
        std::unique_lock lk (mutex);
        const std::vector<Collected>& collected = items;
        return cv.wait_for (lk, timeout, [&] { return pred (collected); });
    }

    [[nodiscard]] std::vector<Collected> getItems() const
    {
        std::lock_guard lk (mutex);
        return items;
    }

    [[nodiscard]] std::size_t count() const
    {
        std::lock_guard lk (mutex);
        return items.size();
    }

    [[nodiscard]] std::size_t count (stills::GenerationStatus s) const
    {
        std::lock_guard lk (mutex);
        std::size_t n = 0;

        for (const auto& i : items)
            n += i.status == s ? 1 : 0;
        return n;
    }

    /// Every succeeded item returned the frame its requested time asks for, on a 30 fps fixture.
    /// Counting completions proves the *accounting*; this proves the *frames*, which is what the
    /// library is for — a batch that delivered the neighbouring frame every time would otherwise
    /// pass every assertion in these tests.
    [[nodiscard]] bool allExact() const { return firstWrong().empty(); }

    /// The first item whose frame is not the one its requested time asks for, as a message; empty
    /// when they all are. Items that did not succeed are not the subject and are skipped.
    [[nodiscard]] std::string firstWrong() const
    {
        std::lock_guard lk (mutex);

        for (const auto& i : items)
        {
            if (i.status != stills::GenerationStatus::succeeded || ! i.index) continue;
            const int want = frameIndexOfTime (i.requested);

            if (*i.index != want)
            {
                return "item " + std::to_string (i.position) + " requested " + stills::toString (i.requested)
                       + " got frame " + std::to_string (*i.index) + ", expected " + std::to_string (want);
            }
        }

        return {};
    }

private:
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<Collected> items;
};

} // namespace testsupport

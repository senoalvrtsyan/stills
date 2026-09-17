#pragma once
// stills/detail/stills_Engine.h — the async machinery: the single worker thread, the batch queue,
// cancellation and teardown. The vocabulary a consumer writes against
// (Completion, CompletionHandler, AsyncRequest, GenerationStatus, WaitResult) is public and lives
// in <stills/stills_Async.h>. See stills_AssetImageGenerator.h for the guarantees this implements.

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <latch>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>
#include <utility>

#include "stills/detail/stills_FramePipeline.h"
#include "stills/detail/stills_UniqueFunction.h"
#include "stills/stills_Async.h"
#include "stills/stills_Error.h"
#include "stills/stills_Image.h"
#include "stills/stills_Time.h"

namespace stills
{

namespace detail
{

// Everything a generator owns. Heap-allocated and never moved: the worker thread and libav
// callbacks hold pointers into it.
struct Engine
{
    Options options;
    std::unique_ptr<FramePipeline> pipeline;
    std::mutex decoderMutex;           // serialises every use of `pipeline`
    std::atomic<int> syncWaiters{ 0 }; // imageAt() callers waiting for the decoder (served before the next async item)

    mutable std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<std::shared_ptr<Batch>> queue; // guarded by queueMutex; front batch is being dispatched
    std::shared_ptr<Batch> current;           // batch of the item in progress; guarded by queueMutex
    bool stopping{ false };                   // guarded by queueMutex
    bool workerDone{ false }; // the worker has drained and left run(); nothing can be delivered any more
    std::atomic<bool> stopFlag{ false };

    // How long the worker defers to waiting synchronous callers before claiming a turn of its own.
    // Bounds the sync-first policy: continuous imageAt() traffic slows a batch to roughly one item
    // per limit, but cannot stop it.
    static constexpr std::chrono::milliseconds syncPriorityLimit{ 20 };

    // Set by the worker once it has deferred for syncPriorityLimit with an item to dispatch, and
    // cleared as soon as that item's decode has released the decoder — before its handler runs, so a
    // handler calling imageAt() on this same thread is not waiting on itself. While it is set,
    // synchronous callers that have not yet taken the decoder wait here instead. Guarded by
    // queueMutex.
    bool workerTurn{ false };
    std::condition_variable turnCv;

    // Two-phase start: the worker's body waits here until the creator has stored `worker`, so
    // nothing the body reads — including AsyncRequest's worker-id check — is still being written.
    std::latch startGate{ 1 };
    std::thread worker; // last member: constructed after everything it touches

    Engine (Options opt, std::unique_ptr<FramePipeline> p) : options (std::move (opt)), pipeline (std::move (p)) {}
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;
    ~Engine() = default; // never joins here: may run on the worker thread itself

    // Publishes a batch. A batch queued while teardown is under way is queued all the same: the
    // worker's drain delivers everything queued as `cancelled`, which is what a completion
    // handler chaining another request during destruction must be able to observe. Only once the
    // worker has drained and exited is there nobody left to deliver: the batch is then marked
    // finished without invoking its handler and this returns false.
    bool enqueue (std::shared_ptr<Batch> b)
    {
        // Recorded before publication so a handler of an *earlier* batch that waits on this one is
        // refused instead of deadlocking (the worker cannot start this batch until that handler
        // returns).
        b->workerId = worker.get_id();
        {
            std::lock_guard lk (queueMutex);

            if (workerDone)
            {
                std::lock_guard blk (b->mutex);
                b->completed = b->times.size();
                b->handler.reset();
                return false;
            }

            queue.push_back (std::move (b));
        }

        queueCv.notify_one();
        return true;
    }

    void cancelAll() noexcept
    {
        std::lock_guard lk (queueMutex);

        for (auto& b : queue)
            b->cancelled.store (true);

        if (current) current->cancelled.store (true);
    }

    void requestStop()
    {
        {
            std::lock_guard lk (queueMutex);
            stopping = true;
            workerTurn = false; // nothing may be left parked on turnCv through teardown

            for (auto& b : queue)
                b->cancelled.store (true);

            if (current) current->cancelled.store (true);
        }

        stopFlag.store (true);
        queueCv.notify_all();
        turnCv.notify_all();
    }

    // Clears a turn claimed by run() and wakes the synchronous callers parked behind it. Takes the
    // flag by reference so a released turn cannot be released twice.
    void releaseTurn (bool& claimed) noexcept
    {
        if (! claimed) return;
        claimed = false;
        {
            std::lock_guard lk (queueMutex);
            workerTurn = false;
        }

        turnCv.notify_all();
    }

    // Worker loop. noexcept: an exception escaping a user handler terminates the process, which is
    // the documented contract (swallowing it would silently break exactly-once accounting).
    //
    // Scheduling: one item of the front batch per turn, with a waiting synchronous imageAt() given
    // the decoder first. A batch stays at the front until its last item has been dispatched, so
    // items of one batch are delivered in request order and the queue is FIFO by batch.
    //
    // The priority is bounded (syncPriorityLimit): synchronous callers arriving back to back keep
    // going first, but once the worker has deferred for that long it takes one item anyway. Without
    // the bound a few threads calling imageAt() in a loop never leave syncWaiters at zero, and
    // because std::mutex grants no fairness they also keep winning the decoder back, so the queue
    // makes almost no progress at all.
    void run() noexcept
    {
        startGate.wait(); // `worker` is fully published

        for (;;)
        {
            // Sync callers first, up to the bound. Skipped when there is nothing queued to defer.
            bool claimedTurn = false;
            {
                std::unique_lock lk (queueMutex);

                if (! queue.empty())
                {
                    if (! queueCv.wait_for (lk, syncPriorityLimit, [&]
                                            { return stopping || syncWaiters.load (std::memory_order_acquire) == 0; }))
                    {
                        workerTurn = true; // deferred long enough; arriving sync callers queue behind it
                        claimedTurn = true;
                    }
                }
            }

            std::shared_ptr<Batch> batch;
            std::size_t index = 0;
            {
                std::unique_lock lk (queueMutex);
                queueCv.wait (lk, [&] { return stopping || ! queue.empty(); });

                if (! queue.empty())
                {
                    batch = queue.front();
                    index = batch->next++;

                    // Off the queue once its last item has been *dispatched*, not completed.
                    if (batch->next >= batch->times.size()) queue.pop_front();
                    current = batch;
                }
            }

            if (! batch)
            {
                releaseTurn (claimedTurn); // nothing was dispatched after all

                // Woken with an empty queue: only reachable once `stopping` is set.
                if (stopFlag.load())
                {
                    // Stopping and drained. A handler run during this drain may have queued more work (the
                    // documented "chain the next request from the handler" pattern meeting destruction), so
                    // re-check under the lock before declaring that nothing can be delivered any more.
                    std::lock_guard lk (queueMutex);

                    if (! queue.empty()) continue;
                    workerDone = true;
                    return;
                }

                continue; // requestStop() has set `stopping` but not yet `stopFlag`
            }

            const CancelToken token{ &batch->cancelled, &stopFlag };
            const Time t = batch->times[index];
            std::expected<Image, Error> result = std::unexpected (Error{ ErrorCode::cancelled, 0, "cancelled" });

            if (! token.isRequested())
            {
                std::unique_lock lk (decoderMutex);
                try
                {
                    result = pipeline->imageAt (t, batch->options, token);
                }
                catch (const std::bad_alloc&)
                {
                    // FramePipeline::imageAt() converts an allocation failure itself, because only it can
                    // put the decoder back into a defined state. This is the guard on the noexcept frame
                    // around it: reaching it would otherwise be std::terminate rather than one failed item.
                    // Only bad_alloc — an exception out of a user handler still terminates, as documented.
                    result = std::unexpected (Error{ ErrorCode::outOfMemory, 0, "out of memory" });
                }

                lk.unlock();
                // std::mutex is not fair; yielding here is a best-effort courtesy so a blocked imageAt()
                // caller usually gets the decoder before the next batch item does.
                std::this_thread::yield();
            }

            // The decoder is free again, so release any claimed turn now — before the handler, not after
            // it. A handler is documented to be able to call imageAt() straight back in, and it runs on
            // this thread: holding the turn across it would park that call on turnCv waiting for a flag
            // only this thread can clear.
            releaseTurn (claimedTurn);
            Completion done{ index, t, std::move (result) };
            done.batch = batch; // so the handler can cancel the rest of its own batch
            batch->handler (std::move (done));
            const bool last = index + 1 == batch->times.size();

            if (last) batch->handler.reset(); // captured state dies on the worker thread, deterministically
            {
                std::lock_guard lk (batch->mutex);
                ++batch->completed;
            }

            if (last) batch->doneCv.notify_all();
            {
                std::lock_guard lk (queueMutex);

                if (current == batch) current.reset();
            }
        }
    }
};

} // namespace detail

} // namespace stills

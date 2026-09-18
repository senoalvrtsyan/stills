#pragma once
// stills/detail/stills_Engine.h — the async machinery: the single worker thread, the batch queue,
// cancellation and teardown. The vocabulary a consumer writes against (Completion,
// CompletionHandler, AsyncRequest, GenerationStatus, WaitResult) is public and lives in
// <stills/stills_Async.h>. See stills_AssetImageGenerator.h for the guarantees this implements.

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
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "stills/detail/stills_FramePipeline.h"
#include "stills/detail/stills_UniqueFunction.h"
#include "stills/stills_Async.h"
#include "stills/stills_Error.h"
#include "stills/stills_Image.h"
#include "stills/stills_Time.h"

namespace stills::detail
{

// Everything a generator owns. Heap-allocated and never moved: the worker thread and libav
// callbacks hold pointers into it.
//
// Two mutexes. `decoderMutex` serialises every use of the pipeline, by the worker and by
// synchronous callers alike. `queueMutex` guards the batch queue and the scheduling state the two
// sides negotiate with (who waits, whose turn it is). A synchronous caller never holds both at once.
class Engine
{
public:
    Engine (Options generatorOptions, std::unique_ptr<FramePipeline> framePipeline)
      : options (std::move (generatorOptions)), pipeline (std::move (framePipeline))
    {
    }

    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;
    ~Engine() = default; // never joins here: may run on the worker thread itself

    // Starts the worker. `self` keeps the engine alive for as long as the thread runs, which is
    // what lets a generator be destroyed from inside one of its own handlers. The thread's body
    // waits until the thread object has been stored, so nothing it reads (AsyncRequest's worker-id
    // check included) is still being written.
    [[nodiscard]] std::expected<void, Error> startWorker (std::shared_ptr<Engine> self)
    {
        assert (self.get() == this && "startWorker() takes the shared_ptr that owns this engine");
#if defined(__cpp_exceptions) && __cpp_exceptions
        try
        {
            worker = std::thread ([self] { self->run(); });
        }
        catch (const std::system_error& e)
        {
            return fail (ErrorCode::internal, std::string ("failed to start worker thread: ") + e.what());
        }
#else
        // -fno-exceptions: std::thread's constructor calls std::terminate instead of throwing, so
        // there is nothing to report and nothing to catch.
        worker = std::thread ([self] { self->run(); });
#endif
        startGate.count_down();
        return {};
    }

    [[nodiscard]] const Options& getOptions() const noexcept { return options; }
    [[nodiscard]] FramePipeline& getPipeline() noexcept { return *pipeline; }
    [[nodiscard]] const FramePipeline& getPipeline() const noexcept { return *pipeline; }

    // True once teardown has begun: no new synchronous request is served.
    [[nodiscard]] bool isStopping() const noexcept { return stopFlag.load(); }

    // The token a synchronous request decodes under: cancelled only by teardown.
    [[nodiscard]] CancelToken makeSyncCancelToken() const noexcept { return CancelToken{ nullptr, &stopFlag }; }

    // Takes the decoder for a synchronous imageAt() call, as a waiting caller the worker defers to.
    //
    // std::mutex grants no fairness, so a caller looping on imageAt() would otherwise keep winning
    // `decoderMutex` back from the worker and a batch running alongside would barely advance. Two
    // things bound that: the worker claims a turn after deferring for syncPriorityLimit, and
    // callers arriving while it holds one wait here until the turn is released. The waiter count is
    // decremented under `queueMutex` because the worker waits on queueCv for "no sync caller
    // waiting", and a notify issued between its predicate check and its block would be lost.
    [[nodiscard]] std::unique_lock<std::mutex> acquireDecoderForSyncCall()
    {
        syncWaiters.fetch_add (1, std::memory_order_acq_rel);
        {
            std::unique_lock queueLock (queueMutex);
            turnCv.wait (queueLock, [&] { return ! workerTurn || stopFlag.load (std::memory_order_acquire); });
        }

        std::unique_lock decoderLock (decoderMutex);
        {
            std::lock_guard queueLock (queueMutex);
            syncWaiters.fetch_sub (1, std::memory_order_acq_rel);
        }

        queueCv.notify_all();
        return decoderLock;
    }

    // Publishes a batch. A batch queued while teardown is under way is queued all the same: the
    // worker's drain delivers everything queued as `cancelled`, which is what a completion
    // handler chaining another request during destruction must be able to observe. Only once the
    // worker has drained and exited is there nobody left to deliver: the batch is then marked
    // finished without invoking its handler and this returns false.
    bool enqueue (std::shared_ptr<Batch> batch)
    {
        // Recorded before publication so a handler of an *earlier* batch that waits on this one is
        // refused instead of deadlocking (the worker cannot start this batch until that handler
        // returns).
        batch->workerId = worker.get_id();
        {
            std::lock_guard lock (queueMutex);

            if (workerDone)
            {
                std::lock_guard batchLock (batch->mutex);
                batch->completed = batch->times.size();
                batch->handler.reset();
                return false;
            }

            queue.push_back (std::move (batch));
        }

        queueCv.notify_one();
        return true;
    }

    void cancelAll() noexcept
    {
        std::lock_guard lock (queueMutex);

        for (auto& batch : queue)
            batch->cancelled.store (true);

        if (current) current->cancelled.store (true);
    }

    // Cancels everything, stops the worker and joins it. Called from the worker itself (a handler
    // destroying its generator) it detaches instead: the worker keeps the engine alive through the
    // shared_ptr it captured and finishes draining, as cancelled, on its own.
    void shutdown() noexcept
    {
        requestStop();

        if (! worker.joinable()) return;
        if (std::this_thread::get_id() == worker.get_id())
            worker.detach();
        else
            worker.join();
    }

private:
    void requestStop() noexcept
    {
        {
            std::lock_guard lock (queueMutex);
            stopping = true;
            workerTurn = false; // nothing may be left parked on turnCv through teardown

            for (auto& batch : queue)
                batch->cancelled.store (true);

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
            std::lock_guard lock (queueMutex);
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
    // going first, but once the worker has deferred for that long it takes one item anyway.
    void run() noexcept
    {
        startGate.wait();

        for (;;)
        {
            bool claimedTurn = false;
            {
                std::unique_lock lock (queueMutex);

                if (! queue.empty())
                {
                    const auto noSyncWaiters = [&]
                    { return stopping || syncWaiters.load (std::memory_order_acquire) == 0; };

                    if (! queueCv.wait_for (lock, syncPriorityLimit, noSyncWaiters))
                    {
                        workerTurn = true; // deferred long enough; arriving sync callers queue behind it
                        claimedTurn = true;
                    }
                }
            }

            std::shared_ptr<Batch> batch;
            std::size_t index = 0;
            {
                std::unique_lock lock (queueMutex);
                queueCv.wait (lock, [&] { return stopping || ! queue.empty(); });

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
                releaseTurn (claimedTurn);

                // Woken with an empty queue: only reachable once `stopping` is set.
                if (stopFlag.load())
                {
                    // A handler run during this drain may have queued more work (the documented "chain the
                    // next request from the handler" pattern meeting destruction), so re-check under the
                    // lock before declaring that nothing can be delivered any more.
                    std::lock_guard lock (queueMutex);

                    if (! queue.empty()) continue;
                    workerDone = true;
                    return;
                }

                continue; // requestStop() has set `stopping` but not yet `stopFlag`
            }

            const CancelToken token{ &batch->cancelled, &stopFlag };
            const Time requestedTime = batch->times[index];
            std::expected<Image, Error> result = std::unexpected (Error{ ErrorCode::cancelled, 0, "cancelled" });

            if (! token.isRequested())
            {
                std::unique_lock lock (decoderMutex);
                try
                {
                    result = pipeline->imageAt (requestedTime, batch->options, token);
                }
                catch (const std::bad_alloc&)
                {
                    // FramePipeline::imageAt() converts an allocation failure itself, because only it can
                    // put the decoder back into a defined state. This is the guard on the noexcept frame
                    // around it: reaching it would otherwise be std::terminate rather than one failed item.
                    // Only bad_alloc: an exception out of a user handler still terminates, as documented.
                    result = std::unexpected (Error{ ErrorCode::outOfMemory, 0, "out of memory" });
                }

                lock.unlock();
                // std::mutex is not fair; yielding here is a best-effort courtesy so a blocked imageAt()
                // caller usually gets the decoder before the next batch item does.
                std::this_thread::yield();
            }

            // The decoder is free again, so release any claimed turn now, before the handler. A handler
            // is documented to be able to call imageAt() straight back in, and it runs on this thread:
            // holding the turn across it would park that call on turnCv waiting for a flag only this
            // thread can clear.
            releaseTurn (claimedTurn);
            Completion completion{ index, requestedTime, std::move (result) };
            completion.batch = batch; // so the handler can cancel the rest of its own batch
            batch->handler (std::move (completion));
            const bool isLast = index + 1 == batch->times.size();

            if (isLast) batch->handler.reset(); // captured state dies on the worker thread, deterministically
            {
                std::lock_guard lock (batch->mutex);
                ++batch->completed;
            }

            if (isLast) batch->doneCv.notify_all();
            {
                std::lock_guard lock (queueMutex);

                if (current == batch) current.reset();
            }
        }
    }

    // How long the worker defers to waiting synchronous callers before claiming a turn of its own.
    // Bounds the sync-first policy: continuous imageAt() traffic slows a batch to roughly one item
    // per limit, but cannot stop it.
    static constexpr std::chrono::milliseconds syncPriorityLimit{ 20 };

    Options options;
    std::unique_ptr<FramePipeline> pipeline;
    std::mutex decoderMutex;
    std::atomic<int> syncWaiters{ 0 }; // imageAt() callers waiting for the decoder (served before the next async item)

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<std::shared_ptr<Batch>> queue; // guarded by queueMutex; front batch is being dispatched
    std::shared_ptr<Batch> current;           // batch of the item in progress; guarded by queueMutex
    bool stopping{ false };                   // guarded by queueMutex
    bool workerDone{ false }; // the worker has drained and left run(); nothing can be delivered any more
    std::atomic<bool> stopFlag{ false };

    // Set by the worker once it has deferred for syncPriorityLimit with an item to dispatch, and
    // cleared as soon as that item's decode has released the decoder. While it is set, synchronous
    // callers that have not yet taken the decoder wait on turnCv. Guarded by queueMutex.
    bool workerTurn{ false };
    std::condition_variable turnCv;

    std::latch startGate{ 1 };
    std::thread worker; // last member: constructed after everything it touches
};

} // namespace stills::detail

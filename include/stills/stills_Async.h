#pragma once
// stills/stills_Async.h — the vocabulary of asynchronous generation: what a completion carries, what a
// handler is, and the handle a batch is observed and cancelled through.
//
// Everything here is used in the signatures of <stills/stills_AssetImageGenerator.h>
// (AssetImageGenerator::generateImages), so it is a public header; <stills/stills_Stills.h> pulls it
// in. The engine that drives it lives in detail/stills_Engine.h.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "stills/detail/stills_UniqueFunction.h"
#include "stills/stills_Error.h"
#include "stills/stills_Image.h"
#include "stills/stills_Options.h"
#include "stills/stills_Time.h"

namespace stills
{

class AssetImageGenerator;

/// Outcome category of one asynchronous item (Apple's AVAssetImageGeneratorResult).
enum class GenerationStatus : std::uint8_t
{
    succeeded,
    failed,
    cancelled
};

[[nodiscard]] constexpr std::string_view toString (GenerationStatus s) noexcept
{
    switch (s)
    {
    case GenerationStatus::succeeded:
        return "succeeded";
    case GenerationStatus::failed:
        return "failed";
    case GenerationStatus::cancelled:
        return "cancelled";
    }

    return "unknown";
}

/// Result of AsyncRequest::wait() / waitFor().
enum class WaitResult : std::uint8_t
{
    finished,              ///< every item of the batch has been delivered
    timedOut,              ///< waitFor() only: the timeout elapsed first
    refusedOnWorkerThread, ///< called from a completion handler of the same generator; waiting
                           ///< there would deadlock, so the call returned at once
};

[[nodiscard]] constexpr std::string_view toString (WaitResult r) noexcept
{
    switch (r)
    {
    case WaitResult::finished:
        return "finished";
    case WaitResult::timedOut:
        return "timedOut";
    case WaitResult::refusedOnWorkerThread:
        return "refusedOnWorkerThread";
    }

    return "unknown";
}

namespace detail
{
struct Batch;
struct Engine;
} // namespace detail

/// Delivered to the completion handler exactly once per requested time.
struct Completion
{
    /// Constructs the item for one requested time. The batch it belongs to is attached by the
    /// engine that delivers it; there is no way to construct a completion already bound to one.
    Completion (std::size_t idx, Time at, std::expected<Image, Error> outcome)
      : index (idx), requestedTime (at), result (std::move (outcome))
    {
    }

    /// Move-only, like the Image it carries: a Completion may be moved out of the handler.
    Completion (Completion&&) noexcept = default;
    Completion& operator= (Completion&&) noexcept = default;

    /// Position of this item in the `times` passed to generateImages().
    std::size_t index;
    Time requestedTime;
    std::expected<Image, Error> result;

    [[nodiscard]] GenerationStatus getStatus() const noexcept
    {
        if (result) return GenerationStatus::succeeded;
        return result.error().code == ErrorCode::cancelled ? GenerationStatus::cancelled : GenerationStatus::failed;
    }

    /// Cancels the rest of the batch this completion belongs to, from inside the handler — the one
    /// thing an AsyncRequest cannot do there, because the handler exists before generateImages()
    /// has returned the handle. Idempotent, never blocks, never runs user code.
    void cancelBatch() const noexcept;

private:
    friend struct detail::Engine;

    // The batch this completion is being delivered from. Set by the engine, read only by
    // cancelBatch(). Weak, not raw: a Completion may be moved out of the handler to keep its Image,
    // and then outlives the batch.
    std::weak_ptr<detail::Batch> batch;
};

/// Move-only callable receiving Completions. May capture move-only state.
using CompletionHandler = detail::UniqueFunction<void (Completion)>;

namespace detail
{

/// One generateImages() call: its times, its handler, and the accounting AsyncRequest observes.
/// Referenced by shared_ptr from the handle, the engine's queue and the item in progress.
struct Batch
{
    std::vector<Time> times;
    CompletionHandler handler;
    RequestOptions options;
    std::atomic<bool> cancelled{ false };
    std::mutex mutex;
    std::condition_variable doneCv;
    std::size_t completed{ 0 }; // guarded by mutex
    std::size_t next{ 0 };      // next item to dispatch; guarded by Engine::queueMutex
    std::thread::id workerId{}; // the generator's worker; set before the batch is published

    [[nodiscard]] bool isFinished() noexcept
    {
        std::lock_guard lk (mutex);
        return completed >= times.size();
    }
};

} // namespace detail

inline void Completion::cancelBatch() const noexcept
{
    if (const std::shared_ptr<detail::Batch> b = batch.lock()) b->cancelled.store (true);
}

/// Observer handle for one generateImages() call. Copyable, and copies share cancellation: they
/// observe one batch, so cancel() on any of them cancels it. Dropping a handle does NOT cancel.
class AsyncRequest
{
public:
    AsyncRequest() noexcept = default;

    /// Requests cancellation of every not-yet-delivered item in this batch. Idempotent, never blocks,
    /// never runs user code. Items already in flight may still complete successfully.
    void cancel() const noexcept
    {
        if (batch) batch->cancelled.store (true);
    }

    /// Blocks until every item has been delivered or the timeout elapses. Returns
    /// `refusedOnWorkerThread` immediately (without waiting) when called from a completion
    /// handler of the same generator: batches run one after another on that thread, so waiting there
    /// would deadlock.
    template <class Rep, class Period>
    [[nodiscard]] WaitResult waitFor (std::chrono::duration<Rep, Period> timeout) const
    {
        if (! batch) return WaitResult::finished;
        if (std::this_thread::get_id() == batch->workerId) return WaitResult::refusedOnWorkerThread;
        std::unique_lock lk (batch->mutex);
        const bool done = batch->doneCv.wait_for (lk, timeout, [&] { return batch->completed >= batch->times.size(); });
        return done ? WaitResult::finished : WaitResult::timedOut;
    }

    /// Blocks until every item has been delivered; see waitFor() for the worker-thread refusal.
    /// Not [[nodiscard]]: waiting for the batch is the point, and the result only distinguishes
    /// `finished` from the worker-thread refusal, which a caller off the worker already knows.
    WaitResult wait() const
    {
        if (! batch) return WaitResult::finished;
        if (std::this_thread::get_id() == batch->workerId) return WaitResult::refusedOnWorkerThread;
        std::unique_lock lk (batch->mutex);
        batch->doneCv.wait (lk, [&] { return batch->completed >= batch->times.size(); });
        return WaitResult::finished;
    }

    [[nodiscard]] bool isFinished() const noexcept { return ! batch || batch->isFinished(); }
    [[nodiscard]] bool isValid() const noexcept { return batch != nullptr; }
    [[nodiscard]] std::size_t getSize() const noexcept { return batch ? batch->times.size() : 0; }
    /// Items delivered so far. Read from inside a handler this does not yet count the delivery in
    /// progress: it is incremented after the handler returns.
    [[nodiscard]] std::size_t getCompleted() const noexcept
    {
        if (! batch) return 0;
        std::lock_guard lk (batch->mutex);
        return batch->completed;
    }

    /// Items not yet delivered.
    [[nodiscard]] std::size_t getRemaining() const noexcept { return getSize() - getCompleted(); }
    /// Same as isValid().
    [[nodiscard]] explicit operator bool() const noexcept { return isValid(); }

private:
    friend class AssetImageGenerator;
    explicit AsyncRequest (std::shared_ptr<detail::Batch> b) noexcept : batch (std::move (b)) {}
    std::shared_ptr<detail::Batch> batch;
};

} // namespace stills

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
STILLS_DEFINE_ENUM_FORMATTER (stills::GenerationStatus);
STILLS_DEFINE_ENUM_FORMATTER (stills::WaitResult);
#endif

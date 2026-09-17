#pragma once
// stills/async.hpp — the vocabulary of asynchronous generation: what a completion carries, what a
// handler is, and the handle a batch is observed and cancelled through.
//
// Everything here is used in the signatures of <stills/generator.hpp>
// (AssetImageGenerator::generate_images), so it is a public header; <stills/stills.hpp> pulls it
// in. The engine that drives it lives in detail/worker.hpp.

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

#include "stills/detail/unique_function.hpp"
#include "stills/error.hpp"
#include "stills/image.hpp"
#include "stills/options.hpp"
#include "stills/time.hpp"

namespace stills {

class AssetImageGenerator;

/// Outcome category of one asynchronous item (Apple's AVAssetImageGeneratorResult).
enum class GenerationStatus : std::uint8_t { succeeded, failed, cancelled };

[[nodiscard]] constexpr std::string_view to_string(GenerationStatus s) noexcept {
  switch (s) {
    case GenerationStatus::succeeded:
      return "succeeded";
    case GenerationStatus::failed:
      return "failed";
    case GenerationStatus::cancelled:
      return "cancelled";
  }
  return "unknown";
}

/// Result of AsyncRequest::wait() / wait_for().
enum class WaitResult : std::uint8_t {
  finished,                  ///< every item of the batch has been delivered
  timed_out,                 ///< wait_for() only: the timeout elapsed first
  refused_on_worker_thread,  ///< called from a completion handler of the same generator; waiting
                             ///< there would deadlock, so the call returned at once
};

[[nodiscard]] constexpr std::string_view to_string(WaitResult r) noexcept {
  switch (r) {
    case WaitResult::finished:
      return "finished";
    case WaitResult::timed_out:
      return "timed_out";
    case WaitResult::refused_on_worker_thread:
      return "refused_on_worker_thread";
  }
  return "unknown";
}

namespace detail {
struct Batch;
struct Engine;
}

/// Delivered to the completion handler exactly once per requested time.
struct Completion {
  /// Constructs the item for one requested time. The batch it belongs to is attached by the
  /// engine that delivers it; there is no way to construct a completion already bound to one.
  Completion(std::size_t idx, Time at, std::expected<Image, Error> outcome)
      : index(idx), requested_time(at), result(std::move(outcome)) {}

  /// Move-only, like the Image it carries: a Completion may be moved out of the handler.
  Completion(Completion&&) noexcept = default;
  Completion& operator=(Completion&&) noexcept = default;

  /// Position of this item in the `times` passed to generate_images().
  std::size_t index;
  Time requested_time;
  std::expected<Image, Error> result;

  [[nodiscard]] GenerationStatus status() const noexcept {
    if (result) return GenerationStatus::succeeded;
    return result.error().code == ErrorCode::cancelled ? GenerationStatus::cancelled
                                                       : GenerationStatus::failed;
  }

  /// Cancels the rest of the batch this completion belongs to, from inside the handler — the one
  /// thing an AsyncRequest cannot do there, because the handler exists before generate_images()
  /// has returned the handle. Idempotent, never blocks, never runs user code.
  void cancel_batch() const noexcept;

 private:
  friend struct detail::Engine;

  // The batch this completion is being delivered from. Set by the engine, read only by
  // cancel_batch(). Weak, not raw: a Completion may be moved out of the handler to keep its Image,
  // and then outlives the batch.
  std::weak_ptr<detail::Batch> batch;
};

/// Move-only callable receiving Completions. May capture move-only state.
using CompletionHandler = detail::unique_function<void(Completion)>;

namespace detail {

/// One generate_images() call: its times, its handler, and the accounting AsyncRequest observes.
/// Referenced by shared_ptr from the handle, the engine's queue and the item in progress.
struct Batch {
  std::vector<Time> times;
  CompletionHandler handler;
  RequestOptions options;
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  std::condition_variable done_cv;
  std::size_t completed{0};     // guarded by mutex
  std::size_t next{0};          // next item to dispatch; guarded by Engine::queue_mutex
  std::thread::id worker_id{};  // the generator's worker; set before the batch is published

  [[nodiscard]] bool finished() noexcept {
    std::lock_guard lk(mutex);
    return completed >= times.size();
  }
};

}  // namespace detail

inline void Completion::cancel_batch() const noexcept {
  if (const std::shared_ptr<detail::Batch> b = batch.lock()) b->cancelled.store(true);
}

/// Observer handle for one generate_images() call. Copyable, and copies share cancellation: they
/// observe one batch, so cancel() on any of them cancels it. Dropping a handle does NOT cancel.
class AsyncRequest {
 public:
  AsyncRequest() noexcept = default;

  /// Requests cancellation of every not-yet-delivered item in this batch. Idempotent, never blocks,
  /// never runs user code. Items already in flight may still complete successfully.
  void cancel() const noexcept {
    if (batch_) batch_->cancelled.store(true);
  }

  /// Blocks until every item has been delivered or the timeout elapses. Returns
  /// `refused_on_worker_thread` immediately (without waiting) when called from a completion
  /// handler of the same generator: batches run one after another on that thread, so waiting there
  /// would deadlock.
  template <class Rep, class Period>
  [[nodiscard]] WaitResult wait_for(std::chrono::duration<Rep, Period> timeout) const {
    if (!batch_) return WaitResult::finished;
    if (std::this_thread::get_id() == batch_->worker_id)
      return WaitResult::refused_on_worker_thread;
    std::unique_lock lk(batch_->mutex);
    const bool done = batch_->done_cv.wait_for(
        lk, timeout, [&] { return batch_->completed >= batch_->times.size(); });
    return done ? WaitResult::finished : WaitResult::timed_out;
  }

  /// Blocks until every item has been delivered; see wait_for() for the worker-thread refusal.
  /// Not [[nodiscard]]: waiting for the batch is the point, and the result only distinguishes
  /// `finished` from the worker-thread refusal, which a caller off the worker already knows.
  WaitResult wait() const {
    if (!batch_) return WaitResult::finished;
    if (std::this_thread::get_id() == batch_->worker_id)
      return WaitResult::refused_on_worker_thread;
    std::unique_lock lk(batch_->mutex);
    batch_->done_cv.wait(lk, [&] { return batch_->completed >= batch_->times.size(); });
    return WaitResult::finished;
  }

  [[nodiscard]] bool finished() const noexcept { return !batch_ || batch_->finished(); }
  [[nodiscard]] bool valid() const noexcept { return batch_ != nullptr; }
  [[nodiscard]] std::size_t size() const noexcept { return batch_ ? batch_->times.size() : 0; }
  /// Items delivered so far. Read from inside a handler this does not yet count the delivery in
  /// progress: it is incremented after the handler returns.
  [[nodiscard]] std::size_t completed() const noexcept {
    if (!batch_) return 0;
    std::lock_guard lk(batch_->mutex);
    return batch_->completed;
  }
  /// Items not yet delivered.
  [[nodiscard]] std::size_t remaining() const noexcept { return size() - completed(); }
  /// Same as valid().
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

 private:
  friend class AssetImageGenerator;
  explicit AsyncRequest(std::shared_ptr<detail::Batch> b) noexcept : batch_(std::move(b)) {}
  std::shared_ptr<detail::Batch> batch_;
};

}  // namespace stills

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
STILLS_DEFINE_ENUM_FORMATTER(stills::GenerationStatus);
STILLS_DEFINE_ENUM_FORMATTER(stills::WaitResult);
#endif

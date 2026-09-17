#pragma once
// stills/generator.hpp — AssetImageGenerator: the public entry point.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include "stills/asset_info.hpp"
#include "stills/async.hpp"
#include "stills/detail/pipeline.hpp"
#include "stills/detail/worker.hpp"
#include "stills/error.hpp"
#include "stills/image.hpp"
#include "stills/options.hpp"
#include "stills/time.hpp"

namespace stills {

/// Extracts still images from a video asset at requested times, synchronously or asynchronously.
///
/// Threading model
///  - One decoder and one worker thread per generator. All decoding is serialised.
///  - image_at() may be called from any thread; concurrent calls queue on the decoder and
///    interleave between asynchronous items.
///  - Completion handlers run on the worker thread, never on the caller's thread. They may call
///    image_at(), generate_images(), cancel_all() and AsyncRequest::cancel(); they must not wait()
///    on their own batch and must not throw.
///  - Destroying the generator cancels pending work, delivers `cancelled` completions for every
///    undelivered item, joins the worker, then frees all libav state. Destroying it from inside a
///    handler is allowed: teardown then completes on the worker thread.
///
/// Deviations from Apple's AVAssetImageGenerator defaults (see README): tolerance defaults to
/// exact, apply_preferred_track_transform defaults to true, and options are fixed at open().
class AssetImageGenerator {
 public:
  /// Opens `source` (a path or any URL libavformat understands). Performs demuxing, stream
  /// selection, decoder + hardware setup and a probe decode of the first frame, so every failure
  /// that can be detected up front is reported here. Options are checked with Options::validate().
  [[nodiscard]] static std::expected<AssetImageGenerator, Error> open(std::string_view source,
                                                                      Options options = {}) {
    if (auto v = check_source(source); !v) return std::unexpected(std::move(v.error()));
    if (auto v = options.validate(); !v) return std::unexpected(std::move(v.error()));
    auto pipeline = detail::FramePipeline::open(std::string{source}, options);
    if (!pipeline) return std::unexpected(std::move(pipeline.error()));
    auto engine = std::make_shared<detail::Engine>(std::move(options), std::move(*pipeline));
#if defined(__cpp_exceptions) && __cpp_exceptions
    try {
      engine->worker = std::thread([eng = engine] { eng->run(); });
    } catch (const std::system_error& e) {
      return detail::fail(ErrorCode::internal,
                          std::string("failed to start worker thread: ") + e.what());
    }
#else
    // -fno-exceptions: std::thread's constructor calls std::terminate instead of throwing, so
    // there is nothing to report and nothing to catch.
    engine->worker = std::thread([eng = engine] { eng->run(); });
#endif
    engine->start_gate.count_down();  // the thread is stored; the body may run
    return AssetImageGenerator{std::move(engine)};
  }

  /// Filesystem overload (paths are UTF-8 on the libavformat side; `path::u8string()` is the
  /// bridge). Constrained to an actual path so string literals and std::string keep taking the URL
  /// overload.
  template <class Path>
    requires std::same_as<std::remove_cvref_t<Path>, std::filesystem::path>
  [[nodiscard]] static std::expected<AssetImageGenerator, Error> open(const Path& source,
                                                                      Options options = {}) {
    const auto u8 = source.u8string();
    return open(std::string_view{reinterpret_cast<const char*>(u8.data()), u8.size()},
                std::move(options));
  }

  AssetImageGenerator(AssetImageGenerator&& other) noexcept : engine_(std::move(other.engine_)) {}
  AssetImageGenerator& operator=(AssetImageGenerator&& other) noexcept {
    if (this != &other) {
      close_impl();
      engine_ = std::move(other.engine_);
    }
    return *this;
  }
  AssetImageGenerator(const AssetImageGenerator&) = delete;
  AssetImageGenerator& operator=(const AssetImageGenerator&) = delete;
  ~AssetImageGenerator() { close_impl(); }

  /// False after being moved from.
  [[nodiscard]] bool is_open() const noexcept { return engine_ != nullptr; }

  /// What was read at open(): dimensions, duration, codec, frame rate, rotation.
  [[nodiscard]] const AssetInfo& info() const noexcept {
    static const AssetInfo empty{};
    return engine_ ? engine_->pipeline->info() : empty;
  }
  /// Which decode path is active. Returned by value because a late hardware failure can switch
  /// the generator to software after open() (see HardwarePolicy); the snapshot is always
  /// consistent.
  [[nodiscard]] ActiveDecoder active_decoder() const {
    return engine_ ? engine_->pipeline->active_decoder() : ActiveDecoder{};
  }
  [[nodiscard]] const Options& options() const noexcept {
    static const Options empty{};
    return engine_ ? engine_->options : empty;
  }
  /// Blocks until the frame for `requested` (asset-relative, zero = first frame) is decoded and
  /// converted. Returns the image together with its actual presentation time
  /// (Image::actual_time()). A waiting caller is served before the worker starts its next
  /// asynchronous item. `options` overrides the tolerance and output box for this call only
  /// (RequestOptions).
  [[nodiscard]] std::expected<Image, Error> image_at(Time requested,
                                                     const RequestOptions& options = {}) {
    if (!engine_) return detail::fail(ErrorCode::invalid_state, "generator has been moved from");
    if (engine_->stop_flag.load())
      return detail::fail(ErrorCode::cancelled, "generator is shutting down");
    const detail::CancelToken token{nullptr, &engine_->stop_flag};
    engine_->sync_waiters.fetch_add(1, std::memory_order_acq_rel);
    {
      // Yield to the worker when it has claimed a turn after being deferred for too long. Without
      // this, callers looping on image_at() keep winning `decoder_mutex` back from it (a
      // std::mutex grants no fairness) and a batch running alongside them barely advances.
      std::unique_lock qlk(engine_->queue_mutex);
      engine_->turn_cv.wait(qlk, [&] {
        return !engine_->worker_turn || engine_->stop_flag.load(std::memory_order_acquire);
      });
    }
    std::unique_lock lk(engine_->decoder_mutex);
    {
      // Decrement under the queue mutex: the worker waits on queue_cv for "no sync caller waiting",
      // and a notify issued between its predicate check and its block would otherwise be lost.
      std::lock_guard qlk(engine_->queue_mutex);
      engine_->sync_waiters.fetch_sub(1, std::memory_order_acq_rel);
    }
    engine_->queue_cv.notify_all();
    return engine_->pipeline->image_at(requested, options, token);
  }

  /// Queues one item per time. Guarantees: exactly one Completion per time, delivered in request
  /// order on the worker thread; undelivered items receive `cancelled` after cancel()/cancel_all()
  /// or destruction; the handler is destroyed on the worker thread after the last delivery. The one
  /// exception is an empty `times`: nothing is queued, the request is already finished, and the
  /// handler is destroyed on the caller's thread.
  ///
  /// Calling this from a completion handler while the generator is being destroyed is allowed: the
  /// batch is queued and the teardown drain delivers every item `cancelled`, so a handler that
  /// chains the next request sees the batch end and stops. Once the worker has drained and exited
  /// there is nobody left to deliver on: the request comes back already finished, the handler is
  /// never invoked and is destroyed on the caller's thread.
  ///
  /// Precondition violations (a moved-from generator, an empty handler) assert in debug builds; in
  /// release builds they return an already-finished request and the handler is never invoked.
  AsyncRequest generate_images(std::span<const Time> times, CompletionHandler handler,
                               const RequestOptions& options = {}) {
    auto batch = std::make_shared<detail::Batch>();
    batch->times.assign(times.begin(), times.end());
    batch->handler = std::move(handler);
    batch->options = options;
    if (!engine_ || batch->times.empty() || !batch->handler) {
      assert(engine_ && "generate_images() on a moved-from generator");
      assert((!engine_ || batch->handler) && "generate_images() with an empty handler");
      batch->completed = batch->times.size();  // finished; handler never invoked
      batch->handler.reset();
      return AsyncRequest{std::move(batch)};
    }
    (void)engine_->enqueue(batch);
    return AsyncRequest{std::move(batch)};
  }

  AsyncRequest generate_images(std::initializer_list<Time> times, CompletionHandler handler,
                               const RequestOptions& options = {}) {
    return generate_images(std::span<const Time>{times.begin(), times.size()}, std::move(handler),
                           options);
  }

  /// Cancels every queued and in-flight asynchronous item.
  /// Non-blocking; the cancelled items are still delivered to their handlers, as `cancelled`.
  void cancel_all() noexcept {
    if (engine_) engine_->cancel_all();
  }

  /// Releases the asset now instead of at the end of the scope: cancels pending work, delivers
  /// `cancelled` for every undelivered item, joins the worker and frees all libav state, exactly as
  /// the destructor does. The generator is then in the moved-from state: is_open() is false,
  /// image_at() reports invalid_state, generate_images() returns an already-finished request
  /// without invoking the handler, and info() / options() / active_decoder() return empty default
  /// values. Images already handed out stay valid. Idempotent.
  void close() noexcept { close_impl(); }

 private:
  explicit AssetImageGenerator(std::shared_ptr<detail::Engine> e) noexcept
      : engine_(std::move(e)) {}

  /// A source string reaches libavformat as a C string, so an embedded NUL would silently open the
  /// prefix — a std::string_view built from a fixed buffer is the usual way that happens.
  [[nodiscard]] static std::expected<void, Error> check_source(std::string_view source) {
    if (source.empty()) return detail::fail(ErrorCode::invalid_argument, "source is empty");
    if (source.find('\0') != std::string_view::npos) {
      return detail::fail(ErrorCode::invalid_argument,
                          "source contains an embedded NUL: it would be truncated at that point");
    }
    return {};
  }

  void close_impl() noexcept {
    if (!engine_) return;
    engine_->request_stop();
    if (engine_->worker.joinable()) {
      if (std::this_thread::get_id() == engine_->worker.get_id()) {
        // Destroyed from inside a handler: the worker keeps the engine alive through its captured
        // shared_ptr and finishes draining (as cancelled) on its own.
        engine_->worker.detach();
      } else {
        engine_->worker.join();
      }
    }
    engine_.reset();
  }

  std::shared_ptr<detail::Engine> engine_;
};

}  // namespace stills

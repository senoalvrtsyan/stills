#pragma once
// stills/stills_AssetImageGenerator.h — AssetImageGenerator: the public entry point.

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

#include "stills/detail/stills_Engine.h"
#include "stills/detail/stills_FramePipeline.h"
#include "stills/stills_AssetInfo.h"
#include "stills/stills_Async.h"
#include "stills/stills_Error.h"
#include "stills/stills_Image.h"
#include "stills/stills_Options.h"
#include "stills/stills_Time.h"

namespace stills
{

/// Extracts still images from a video asset at requested times, synchronously or asynchronously.
///
/// Threading model
///  - One decoder and one worker thread per generator. All decoding is serialised.
///  - imageAt() may be called from any thread; concurrent calls queue on the decoder and
///    interleave between asynchronous items.
///  - Completion handlers run on the worker thread, never on the caller's thread. They may call
///    imageAt(), generateImages(), cancelAll() and AsyncRequest::cancel(); they must not wait()
///    on their own batch and must not throw.
///  - Destroying the generator cancels pending work, delivers `cancelled` completions for every
///    undelivered item, joins the worker, then frees all libav state. Destroying it from inside a
///    handler is allowed: teardown then completes on the worker thread.
///
/// Deviations from Apple's AVAssetImageGenerator defaults (see README): tolerance defaults to
/// exact, applyPreferredTrackTransform defaults to true, and options are fixed at open().
class AssetImageGenerator
{
public:
    /// Opens `source` (a path or any URL libavformat understands). Performs demuxing, stream
    /// selection, decoder + hardware setup and a probe decode of the first frame, so every failure
    /// that can be detected up front is reported here. Options are checked with Options::validate().
    [[nodiscard]] static std::expected<AssetImageGenerator, Error> open (std::string_view source, Options options = {})
    {
        if (auto v = checkSource (source); ! v) return std::unexpected (std::move (v.error()));
        if (auto v = options.validate(); ! v) return std::unexpected (std::move (v.error()));
        auto pipeline = detail::FramePipeline::open (std::string{ source }, options);

        if (! pipeline) return std::unexpected (std::move (pipeline.error()));
        auto engine = std::make_shared<detail::Engine> (std::move (options), std::move (*pipeline));
#if defined(__cpp_exceptions) && __cpp_exceptions
        try
        {
            engine->worker = std::thread ([eng = engine] { eng->run(); });
        }
        catch (const std::system_error& e)
        {
            return detail::fail (ErrorCode::internal, std::string ("failed to start worker thread: ") + e.what());
        }
#else
        // -fno-exceptions: std::thread's constructor calls std::terminate instead of throwing, so
        // there is nothing to report and nothing to catch.
        engine->worker = std::thread ([eng = engine] { eng->run(); });
#endif
        engine->startGate.count_down(); // the thread is stored; the body may run
        return AssetImageGenerator{ std::move (engine) };
    }

    /// Filesystem overload (paths are UTF-8 on the libavformat side; `path::u8string()` is the
    /// bridge). Constrained to an actual path so string literals and std::string keep taking the URL
    /// overload.
    template <class Path>
        requires std::same_as<std::remove_cvref_t<Path>, std::filesystem::path>
    [[nodiscard]] static std::expected<AssetImageGenerator, Error> open (const Path& source, Options options = {})
    {
        const auto u8 = source.u8string();
        return open (std::string_view{ reinterpret_cast<const char*> (u8.data()), u8.size() }, std::move (options));
    }

    AssetImageGenerator (AssetImageGenerator&& other) noexcept : engine (std::move (other.engine)) {}
    AssetImageGenerator& operator= (AssetImageGenerator&& other) noexcept
    {
        if (this != &other)
        {
            closeImpl();
            engine = std::move (other.engine);
        }

        return *this;
    }

    AssetImageGenerator (const AssetImageGenerator&) = delete;
    AssetImageGenerator& operator= (const AssetImageGenerator&) = delete;
    ~AssetImageGenerator() { closeImpl(); }

    /// False after being moved from.
    [[nodiscard]] bool isOpen() const noexcept { return engine != nullptr; }

    /// What was read at open(): dimensions, duration, codec, frame rate, rotation.
    [[nodiscard]] const AssetInfo& getInfo() const noexcept
    {
        static const AssetInfo empty{};
        return engine ? engine->pipeline->getInfo() : empty;
    }

    /// Which decode path is active. Returned by value because a late hardware failure can switch
    /// the generator to software after open() (see HardwarePolicy); the snapshot is always
    /// consistent.
    [[nodiscard]] ActiveDecoder getActiveDecoder() const
    {
        return engine ? engine->pipeline->getActiveDecoder() : ActiveDecoder{};
    }

    [[nodiscard]] const Options& getOptions() const noexcept
    {
        static const Options empty{};
        return engine ? engine->options : empty;
    }

    /// Blocks until the frame for `requested` (asset-relative, zero = first frame) is decoded and
    /// converted. Returns the image together with its actual presentation time
    /// (Image::getActualTime()). A waiting caller is served before the worker starts its next
    /// asynchronous item. `options` overrides the tolerance and output box for this call only
    /// (RequestOptions).
    [[nodiscard]] std::expected<Image, Error> imageAt (Time requested, const RequestOptions& options = {})
    {
        if (! engine) return detail::fail (ErrorCode::invalidState, "generator has been moved from");
        if (engine->stopFlag.load()) return detail::fail (ErrorCode::cancelled, "generator is shutting down");
        const detail::CancelToken token{ nullptr, &engine->stopFlag };
        engine->syncWaiters.fetch_add (1, std::memory_order_acq_rel);
        {
            // Yield to the worker when it has claimed a turn after being deferred for too long. Without
            // this, callers looping on imageAt() keep winning `decoderMutex` back from it (a
            // std::mutex grants no fairness) and a batch running alongside them barely advances.
            std::unique_lock qlk (engine->queueMutex);
            engine->turnCv.wait (qlk, [&]
                                 { return ! engine->workerTurn || engine->stopFlag.load (std::memory_order_acquire); });
        }

        std::unique_lock lk (engine->decoderMutex);
        {
            // Decrement under the queue mutex: the worker waits on queueCv for "no sync caller waiting",
            // and a notify issued between its predicate check and its block would otherwise be lost.
            std::lock_guard qlk (engine->queueMutex);
            engine->syncWaiters.fetch_sub (1, std::memory_order_acq_rel);
        }

        engine->queueCv.notify_all();
        return engine->pipeline->imageAt (requested, options, token);
    }

    /// Queues one item per time. Guarantees: exactly one Completion per time, delivered in request
    /// order on the worker thread; undelivered items receive `cancelled` after cancel()/cancelAll()
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
    AsyncRequest generateImages (std::span<const Time> times, CompletionHandler handler,
                                 const RequestOptions& options = {})
    {
        auto batch = std::make_shared<detail::Batch>();
        batch->times.assign (times.begin(), times.end());
        batch->handler = std::move (handler);
        batch->options = options;

        if (! engine || batch->times.empty() || ! batch->handler)
        {
            assert (engine && "generateImages() on a moved-from generator");
            assert ((! engine || batch->handler) && "generateImages() with an empty handler");
            batch->completed = batch->times.size(); // finished; handler never invoked
            batch->handler.reset();
            return AsyncRequest{ std::move (batch) };
        }

        (void)engine->enqueue (batch);
        return AsyncRequest{ std::move (batch) };
    }

    AsyncRequest generateImages (std::initializer_list<Time> times, CompletionHandler handler,
                                 const RequestOptions& options = {})
    {
        return generateImages (std::span<const Time>{ times.begin(), times.size() }, std::move (handler), options);
    }

    /// Cancels every queued and in-flight asynchronous item.
    /// Non-blocking; the cancelled items are still delivered to their handlers, as `cancelled`.
    void cancelAll() noexcept
    {
        if (engine) engine->cancelAll();
    }

    /// Releases the asset now instead of at the end of the scope: cancels pending work, delivers
    /// `cancelled` for every undelivered item, joins the worker and frees all libav state, exactly as
    /// the destructor does. The generator is then in the moved-from state: isOpen() is false,
    /// imageAt() reports invalidState, generateImages() returns an already-finished request
    /// without invoking the handler, and getInfo() / getOptions() / getActiveDecoder() return empty default
    /// values. Images already handed out stay valid. Idempotent.
    void close() noexcept { closeImpl(); }

private:
    explicit AssetImageGenerator (std::shared_ptr<detail::Engine> e) noexcept : engine (std::move (e)) {}

    // A source string reaches libavformat as a C string, so an embedded NUL would silently open the
    // prefix — a std::string_view built from a fixed buffer is the usual way that happens.
    [[nodiscard]] static std::expected<void, Error> checkSource (std::string_view source)
    {
        if (source.empty()) return detail::fail (ErrorCode::invalidArgument, "source is empty");
        if (source.find ('\0') != std::string_view::npos)
        {
            return detail::fail (ErrorCode::invalidArgument,
                                 "source contains an embedded NUL: it would be truncated at that point");
        }

        return {};
    }

    void closeImpl() noexcept
    {
        if (! engine) return;
        engine->requestStop();

        if (engine->worker.joinable())
        {
            if (std::this_thread::get_id() == engine->worker.get_id())
            {
                // Destroyed from inside a handler: the worker keeps the engine alive through its captured
                // shared_ptr and finishes draining (as cancelled) on its own.
                engine->worker.detach();
            }
            else
            {
                engine->worker.join();
            }
        }

        engine.reset();
    }

    std::shared_ptr<detail::Engine> engine;
};

} // namespace stills

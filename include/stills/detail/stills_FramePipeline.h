#pragma once
// stills/detail/stills_FramePipeline.h — the orchestrator. Owns the six decode types (MediaSource,
// PacketReader, KeyframeIndex, VideoDecoder, Positioner, FrameSelector) and is the only one of them
// that seeks, re-opens or rebuilds the decoder, because each of those invalidates the state of
// every other type at once. What is left here besides that sequencing: the request contract
// (validation and the hardware-fault ladder), the decoder ladder at open, and conversion of the
// chosen frame into the output Image.
//
// Single-threaded by contract: callers serialise access (see stills_Engine.h).

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stills/detail/stills_AssetInfoBuilder.h"
#include "stills/detail/stills_Converter.h"
#include "stills/detail/stills_DecodeFrontier.h"
#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_FrameSelector.h"
#include "stills/detail/stills_HardwareSupport.h"
#include "stills/detail/stills_KeyframeIndex.h"
#include "stills/detail/stills_MediaSource.h"
#include "stills/detail/stills_PacketReader.h"
#include "stills/detail/stills_Positioner.h"
#include "stills/detail/stills_RequestWindow.h"
#include "stills/detail/stills_SeekCostModel.h"
#include "stills/detail/stills_VideoDecoder.h"
#include "stills/stills_AssetInfo.h"
#include "stills/stills_Image.h"
#include "stills/stills_Options.h"
#include "stills/stills_Time.h"

namespace stills::detail
{

class FramePipeline
{
public:
    using Selected = FrameSelector::Selected;

    FramePipeline (const FramePipeline&) = delete;
    FramePipeline& operator= (const FramePipeline&) = delete;
    FramePipeline (FramePipeline&&) = delete;
    FramePipeline& operator= (FramePipeline&&) = delete;
    ~FramePipeline() = default;

    // Opens the source. `token` (optional) makes the open cancellable: libavformat's interrupt
    // callback consults it during probing and the first decode, so a stalled network source fails
    // with `cancelled` / `openFailed` instead of blocking.
    [[nodiscard]] static std::expected<std::unique_ptr<FramePipeline>, Error> open (std::string source, Options options,
                                                                                    const CancelToken* token = nullptr)
    {
        std::unique_ptr<FramePipeline> pipeline{ new FramePipeline (std::move (source), std::move (options)) };
        pipeline->source.setCancelToken (token);
        auto finish = [&] (Error error) -> std::expected<std::unique_ptr<FramePipeline>, Error>
        {
            pipeline->source.setCancelToken (nullptr);

            if (token != nullptr && token->isRequested()) return fail (ErrorCode::cancelled, "open cancelled");
            return std::unexpected (std::move (error));
        };

        if (auto result = pipeline->source.open (pipeline->options); ! result)
            return finish (std::move (result.error()));
        if (auto result = pipeline->attachToSource(); ! result) return finish (std::move (result.error()));
        if (auto result = pipeline->setupDecoder(); ! result) return finish (std::move (result.error()));
        pipeline->info = makeAssetInfo (pipeline->source, pipeline->decoder, *pipeline->converter, pipeline->probeFrame,
                                        pipeline->options);
        pipeline->source.setCancelToken (nullptr);
        return pipeline;
    }

    [[nodiscard]] const AssetInfo& getInfo() const noexcept { return info; }

    // Snapshot of the active decode path. Returned by value: a lazy hardware failure may switch
    // the path to software after open(), and readers must never observe a torn value. The only
    // member function of this class another thread may call (see stills_VideoDecoder.h).
    [[nodiscard]] ActiveDecoder getActiveDecoder() const { return decoder.getActiveDecoder(); }

    // Seeks and decoded frames attributable to the request that just returned. Both are differences
    // of the lifetime counters MediaSource and VideoDecoder keep where the calls are made, so
    // reading them adds no work to the decode path. The benchmark harness (tests/bench) uses them
    // to check that a restructure changes neither count.
    [[nodiscard]] int getSeekCount() const noexcept
    {
        return static_cast<int> (source.getSeekCallCount() - requestStats.seeksAtStart);
    }

    [[nodiscard]] int getDecodedFrameCount() const noexcept
    {
        return static_cast<int> (decoder.getReceiveCount() - requestStats.framesAtStart);
    }

    // Extracts the frame for `requested` (asset-relative). Thread-unsafe by design.
    [[nodiscard]] std::expected<Image, Error> imageAt (Time requested, const CancelToken& token)
    {
        return imageAt (requested, RequestOptions{}, token);
    }

    // The decode path allocates freely and the async worker's loop is noexcept, so a std::bad_alloc
    // below here would terminate instead of surfacing the outOfMemory the API defines. Caught at
    // this frame, the only one that can drop the half-updated state: the next request re-positions.
    [[nodiscard]] std::expected<Image, Error> imageAt (Time requested, const RequestOptions& requestOptions,
                                                       const CancelToken& token)
    {
        requestStats =
            RequestStats{ .seeksAtStart = source.getSeekCallCount(), .framesAtStart = decoder.getReceiveCount() };
        try
        {
            return imageAtImpl (requested, requestOptions, token);
        }
        catch (const std::bad_alloc&)
        {
            source.setCancelToken (nullptr);
            resetPosition();        // frees the frames and the buffers; allocates nothing
            position.markInvalid(); // an unwound request left the demuxer wherever it stopped
            decoder.clearHardwareFault();
            // Short enough to live in the string's own storage, so building it cannot allocate either.
            return std::unexpected (Error{ ErrorCode::outOfMemory, 0, "out of memory" });
        }
    }

private:
    // Where the sub-objects' lifetime counters stood when the current request started. Feeds
    // SeekCostModel::learn() and the seek-versus-decode-forward decision.
    struct RequestStats
    {
        std::int64_t seeksAtStart{ 0 };
        std::int64_t framesAtStart{ 0 }; // counts look-ahead and skipped frames, like the counter it comes from
    };

    FramePipeline (std::string sourcePath, Options generatorOptions)
      : source (std::move (sourcePath)), options (std::move (generatorOptions))
    {
    }

    // ---- the request -------------------------------------------------------------------------

    [[nodiscard]] std::expected<Image, Error> imageAtImpl (Time requested, const RequestOptions& requestOptions,
                                                           const CancelToken& token)
    {
        source.setCancelToken (&token);
        // Only a fault raised by *this* request may drive the rebuild below. One left over from a
        // candidate probed at open would otherwise turn the next unrelated failure (a cancellation, a
        // timeOutOfRange) into a decoder rebuild and rewrite fallbackReason.
        decoder.clearHardwareFault();
        const auto attempt = [&]() -> std::expected<Image, Error>
        {
            auto selected = selectFor (requested, requestOptions, token);

            if (! selected) return std::unexpected (std::move (selected.error()));
            return convert (*selected);
        };

        auto result = attempt();

        if (shouldRebuildHardwareOnce (result))
        {
            decoder.clearHardwareFault();
            decoder.noteHardwareRetry();
            const std::string failure = result.error().message;

            if (auto rebuilt = rebuildHardware(); rebuilt)
            {
                result = attempt();
            }
            else
            {
                decoder.noteHardwareFault();
                result = std::unexpected (makeError (
                    ErrorCode::decodeFailed, 0, failure + "; hardware rebuild failed: " + rebuilt.error().message));
            }
        }

        if (shouldFallBackToSoftware (result))
        {
            decoder.clearHardwareFault();

            if (options.hardware.policy == HardwarePolicy::requireHardware)
            {
                source.setCancelToken (nullptr);
                return std::unexpected (makeError (ErrorCode::hardwareUnavailable, 0,
                                                   "hardware decoding failed: " + result.error().message));
            }

            if (auto rebuilt = rebuildSoftware ("hardware decoder failed during decoding: " + result.error().message);
                ! rebuilt)
            {
                source.setCancelToken (nullptr);
                return std::unexpected (std::move (rebuilt.error()));
            }

            result = attempt();
        }

        if (result && decoder.isHardwareActive()) decoder.rearmHardwareRetry();
        source.setCancelToken (nullptr);

        // AVERROR_EXIT with a cancelled token is the cancellation; with a live token it is a stale
        // interrupt and stays an I/O failure.
        if (! result && result.error().avError == libav::exitRequested && token.isRequested())
        {
            return fail (ErrorCode::cancelled, "cancelled");
        }

        return result;
    }

    // Hardware faults after open (an EIO on a surface download, a lost session) are usually
    // transient and clear on a fresh session, so the same hardware decoder is rebuilt once per run
    // of failures before falling back to software.
    [[nodiscard]] bool shouldRebuildHardwareOnce (const std::expected<Image, Error>& result) const noexcept
    {
        if (result || ! decoder.hasHardwareFault()) return false;
        return decoder.isHardwareActive() && ! decoder.hasRetriedHardware();
    }

    [[nodiscard]] bool shouldFallBackToSoftware (const std::expected<Image, Error>& result) const noexcept
    {
        return ! result && decoder.hasHardwareFault();
    }

    // Validates the request, maps it into the stream's timestamp domain, makes sure the source is
    // usable, and positions and selects the frame without converting it (the frame stays owned by
    // the selector, in one of its FrameSlots).
    [[nodiscard]] std::expected<Selected, Error> selectFor (Time requested, const RequestOptions& requestOptions,
                                                            const CancelToken& token)
    {
        if (auto validation = requestOptions.validate (options.pixelFormat); ! validation)
        {
            return std::unexpected (std::move (validation.error()));
        }

        requestedMaximumSize = requestOptions.maximumSize;
        auto window = RequestWindow::resolve (requested, requestOptions.tolerance.value_or (options.tolerance),
                                              getStreamInfo(), options.outOfRange);

        if (! window) return std::unexpected (std::move (window.error()));
        if (! source.isOpen() || ! decoder.isBuilt())
        {
            // A previous re-open failed (or was cancelled): retry rather than staying dead forever.
            if (auto result = reopen(); ! result)
            {
                return fail (ErrorCode::unusable, result.error().avError, "the generator is unusable: " + brokenReason);
            }
        }

        if (! recoverAfterInterrupt())
        {
            // The interrupt left libavio's state stuck and this libavformat major is not one
            // MediaSource::tryResetIoState() can clear: re-open instead of reading through it.
            if (auto result = reopen(); ! result) return std::unexpected (std::move (result.error()));
        }

        selector.beginAttempt();
        return extract (*window, token);
    }

    // Positions and selects (the caller converts). Every frame-selection decision is the
    // selector's; what is done here is the sequencing, and the positioning it asks for.
    [[nodiscard]] std::expected<Selected, Error> extract (RequestWindow window, const CancelToken& token)
    {
        if (auto fromHeld = selector.answerFromHeld (window, makeSelectionContext(), token))
            return std::move (*fromHeld);
        selector.beginRequest (window, makeSelectionContext());
        const std::int64_t target = window.getTarget();

        if (window.isKeyframeMode())
        {
            if (auto result = positionFor (target, true, token); ! result)
                return std::unexpected (std::move (result.error()));
            if (selector.isPreEditKeyframe (makeSelectionContext()))
            {
                // The keyframe covering the target precedes the first presented frame (an edit list
                // trimmed its GOP). Answer with the first presented frame, clamped, decoded exactly: the
                // decoder drops the trimmed frames itself (AV_PKT_FLAG_DISCARD).
                selector.useExactMode();
                window = window.collapsedTo (firstFrameTs != libav::noPts ? firstFrameTs : getStreamInfo().startPts,
                                             Adjustment::keyframeBeforeEdit);
            }
            else if (auto result = armKeyframeDecode (token); ! result)
            {
                return std::unexpected (std::move (result.error()));
            }
        }
        else if (selector.canPromotePendingFrame (target, makeSelectionContext()))
        {
            selector.promotePending();
            positioner.resetBackoff();
        }
        else if (selector.canContinueFromHeldFrame (target, makeSelectionContext()))
        {
            positioner.resetBackoff();
        }
        else if (auto result = positionFor (target, false, token); ! result)
        {
            return std::unexpected (std::move (result.error()));
        }

        costs.beginRequest();
        const int framesBefore = getDecodedFrameCount();
        const bool seeked = getSeekCount() > 0;
        auto selected = runSelection (window, token);

        if (! selected)
        {
            // A request abandoned before reaching its target fed packets under *its* skip window, and
            // only it knew where that window was: the frames between the decoder's frontier and the
            // window may never be produced, so nothing may continue forward across it. The next request
            // repositions, one seek per cancellation. A source that cannot be repositioned (a pipe)
            // relies on the recorded holes instead, and a request across one fails rather than lies.
            if (selector.hasSkipWindow() && getStreamInfo().seekable) position.markInvalid();
            return std::unexpected (std::move (selected.error()));
        }

        costs.learn (getDecodedFrameCount() - framesBefore, seeked);

        if (selected->adjustment == Adjustment::none) selected->adjustment = window.getAdjustment();
        if (window.isKeyframeMode())
            return selector.verifyKeyframeTail (*selected, window, makeSelectionContext(), token);
        return *selected;
    }

    // Runs the selection loop to an answer, carrying out the repositions it asks for in between.
    // The selector never seeks; this is the one place its two requests are turned into seeks.
    [[nodiscard]] std::expected<Selected, Error> runSelection (const RequestWindow& window, const CancelToken& token)
    {
        for (;;)
        {
            FrameSelector::Step step = selector.select (window, makeSelectionContext(), token);

            switch (step.action)
            {
            case FrameSelector::Step::Action::done:
                return std::move (step.result);
            case FrameSelector::Step::Action::backOff:
                if (auto moved = backOff (window.getTarget(), step.observedKey); ! moved)
                    return std::unexpected (std::move (moved.error()));
                break;
            case FrameSelector::Step::Action::retryAfterEmptyEof:
                if (auto moved = retryAfterEmptyEof (window.getTarget(), token); ! moved)
                    return std::unexpected (std::move (moved.error()));
                break;
            }
        }
    }

    // Keyframe mode after positioning: make sure the keyframe packet is pending, then let the
    // selector feed it and drain. Fetching it may read a landing, which may seek, so that half is
    // here.
    [[nodiscard]] std::expected<void, Error> armKeyframeDecode (const CancelToken& token)
    {
        if (! packetReader.isPacketPending())
        {
            auto landing = readLanding (std::numeric_limits<std::int64_t>::max(), /*scan=*/false,
                                        /*keyframeMode=*/true, token);

            if (! landing) return std::unexpected (std::move (landing.error()));
            if (! packetReader.isPacketPending()) return {}; // nothing to feed (end of stream); select() reports it
        }

        return selector.feedKeyframeAndDrain (makeSelectionContext());
    }

    [[nodiscard]] const StreamInfo& getStreamInfo() const noexcept { return source.getStreamInfo(); }

    // What a selection step reads and drives, gathered fresh at every call: a re-open replaces the
    // AVStream underneath the source, so nothing built here may outlive the call it is handed to.
    [[nodiscard]] SelectionContext makeSelectionContext() noexcept
    {
        return SelectionContext{ source, decoder,    packetReader, keyframes,         position,
                                 costs,  positioner, firstFrameTs, options.outOfRange };
    }

    // What a positioning decision reads, gathered fresh at every call, for the same reason.
    [[nodiscard]] PositioningView makePositioningView() const noexcept
    {
        return PositioningView{ getStreamInfo(), keyframes, selector.getFrontier(),
                                costs,           position,  containerIndexOf (source) };
    }

    // ---- opening and the decoder ladder ------------------------------------------------------

    // The pipeline's own half of opening: the decoder libavformat picked for the stream, the packet
    // and frame buffers, and the converter (whose orientation comes from the stream's display
    // matrix). Runs after every MediaSource open and re-open, and owns nothing MediaSource owns.
    [[nodiscard]] std::expected<void, Error> attachToSource()
    {
        if (auto result = decoder.attach (source.getCodec()); ! result) return result;
        if (auto result = packetReader.attach(); ! result) return result;
        if (auto result = selector.attach(); ! result) return result;
        const DisplayTransform applied =
            options.applyPreferredTrackTransform ? getStreamInfo().transform : DisplayTransform{};
        converter.emplace (options.pixelFormat, options.scaler, options.maximumSize, options.applySampleAspectRatio,
                           applied.rotation, applied.mirrored,
                           /*removeDisplayMatrix=*/options.applyPreferredTrackTransform);
        return {};
    }

    // Builds (or replaces) the decoder for the stream, and puts the pipeline back to knowing
    // nothing: a frame decoded by the decoder being replaced says nothing about where its
    // replacement is. The drop happens before the build rather than inside it, so the surfaces a
    // hardware decoder handed us are given back before its pool is.
    [[nodiscard]] std::expected<void, Error> buildCodec (const HwCandidate* candidate)
    {
        resetPosition(); // including the recorded holes: a new decoder produces them all again
        const AVStream* stream = source.getStream();

        if (auto result = decoder.build (*stream->codecpar, stream->time_base, options, candidate); ! result)
        {
            return result;
        }

        // Raw elementary streams carry no timestamps of their own, so the frame rate the decoder
        // negotiated is the only one there is.
        if (getStreamInfo().synthesizeTimestamps)
        {
            const AVRational frameRate = decoder.getFrameRate();

            if (frameRate.num > 0 && frameRate.den > 0) source.adoptDecoderFrameRate (frameRate);
            if (getStreamInfo().frameDurationHint <= 0)
            {
                source.adoptDecoderFrameRate (AVRational{ 25, 1 }); // libav's own default
            }
        }

        return {};
    }

    // Tries the hardware candidates in preference order, then software. Each attempt is validated
    // by decoding the first frame so lazy failures surface here, not on the first request.
    [[nodiscard]] std::expected<void, Error> setupDecoder()
    {
        std::string reason;
        Options effective = options;

        if (effective.hardware.policy == HardwarePolicy::automatic)
        {
            const bool worthwhile =
                VideoDecoder::isHardwareWorthwhile (*source.getStream()->codecpar, *source.getCodec());
            effective.hardware.policy = worthwhile ? HardwarePolicy::preferHardware : HardwarePolicy::softwareOnly;
        }

        std::vector<HwCandidate> candidates = hwCandidates (source.getCodec(), effective, reason);

        if (options.hardware.policy == HardwarePolicy::automatic
            && effective.hardware.policy == HardwarePolicy::softwareOnly)
        {
            const AVCodecParameters& parameters = *source.getStream()->codecpar;
            reason = "automatic policy: software decoding is faster for " + std::to_string (parameters.width) + "x"
                     + std::to_string (parameters.height) + " " + decoder.getCodecName();
        }

        if (! candidates.empty() && ! getStreamInfo().ioSeekable)
        {
            // Probing a candidate decodes the first frame and a rejected one needs the input rewound;
            // a pipe cannot be rewound, so the probe would consume it. Software needs exactly one pass.
            candidates.clear();
            reason = "non-rewindable input: hardware probing would consume it";
        }

        std::string attempts;

        for (const HwCandidate& candidate : candidates)
        {
            auto buildResult = buildCodec (&candidate);
            std::expected<void, Error> probeResult = buildResult ? probeFirstFrame() : std::expected<void, Error>{};

            if (buildResult && probeResult && decoder.didAcceptHardwareFormat())
            {
                decoder.publishHardware (options.hardware.device);
                return {};
            }

            std::string failure = "decoder declined the hardware pixel format (profile not supported)";

            if (! buildResult)
                failure = buildResult.error().message;
            else if (! probeResult)
                failure = probeResult.error().message;

            if (! attempts.empty()) attempts += "; ";
            attempts += VideoDecoder::getHardwareTypeName (candidate.type) + ": " + failure;
        }

        if (options.hardware.policy == HardwarePolicy::requireHardware)
        {
            return fail (ErrorCode::hardwareUnavailable,
                         "no hardware decoder could be used (" + (attempts.empty() ? reason : attempts) + ")");
        }

        if (auto result = buildCodec (nullptr); ! result) return result;
        if (auto result = probeFirstFrame(); ! result) return result;
        decoder.publishSoftware (attempts.empty() ? reason : attempts);
        return {};
    }

    // Re-creates the active hardware decoder (same device type and format) and re-probes it.
    [[nodiscard]] std::expected<void, Error> rebuildHardware()
    {
        const std::optional<HwCandidate> candidate = decoder.getHardwareCandidate();

        if (! candidate) return fail (ErrorCode::internal, "no hardware decoder to rebuild");
        if (auto result = buildCodec (&*candidate); ! result) return result;
        if (auto probed = probeFirstFrame(); ! probed) return probed;
        if (! decoder.didAcceptHardwareFormat())
        {
            return fail (ErrorCode::hardwareUnavailable, "rebuilt decoder declined the hardware format");
        }

        return {};
    }

    [[nodiscard]] std::expected<void, Error> rebuildSoftware (std::string reason)
    {
        auto result = buildCodec (nullptr);

        if (! result)
        {
            brokenReason = "software decoder could not be rebuilt after a hardware failure: " + result.error().message;
            return result;
        }

        // Report the software decoder even if the probe below fails: an I/O error there leaves a
        // working software path whose first request simply failed.
        decoder.publishSoftware (reason);

        if (auto probed = probeFirstFrame(); ! probed)
        {
            decoder.publishSoftware (std::move (reason) + "; probe after rebuild failed: " + probed.error().message);
            return probed;
        }

        return {};
    }

    // Decodes the first frame of the stream (any tolerance) and remembers its timestamp. Always
    // goes through the explicit start seek, so on containers whose seek does not land on
    // keyframes (MPEG-TS) the "first frame" really is the first frame, for every candidate.
    [[nodiscard]] std::expected<void, Error> probeFirstFrame()
    {
        CancelToken none;
        selector.beginProbe();

        if (position.isPositioned())
        {
            if (auto result = seekToStart(); ! result) return result;
        }
        else
        {
            position.markAtStart (getStreamInfo().startPts);
        }

        auto selected = runSelection (RequestWindow::anyFrameFrom (getStreamInfo().startPts), none);

        if (! selected)
        {
            if (decoder.isHardwareActive())
                return fail (ErrorCode::hardwareUnavailable, "first frame: " + selected.error().message);
            return std::unexpected (std::move (selected.error()));
        }

        if (decoder.isHardwareActive() && selected->frame->format != decoder.getHardwarePixelFormat())
        {
            return fail (ErrorCode::hardwareUnavailable, "decoder produced a software frame");
        }

        firstFrameTs = selector.getFrameTs (*selected->frame, getStreamInfo());
        probeFrame = selected->frame;

        // Raw streams: anchor Time::zero() at the first frame.
        if (source.needsStartPtsFromFirstFrame()) source.setStartPts (firstFrameTs);
        return {};
    }

    // ---- positioning as an action ------------------------------------------------------------
    //
    // A seek is a hint: where it landed is established from the first keyframe packet after it,
    // before anything is decoded. Containers with a trusted index (MP4, Matroska) correct an
    // overshoot with one re-seek to the previous keyframe; containers without one (MPEG-TS) aim a
    // GOP early and scan packets up to the target, recording every keyframe (time, byte position,
    // GOP extent) in a lazily built index that later requests position from with one byte seek.
    // Decoding verifies the landing again, for demuxers whose keyframe flags cannot be trusted.
    //
    // Where to aim is Positioner's and how the landing reads is PacketReader's; what is here is the
    // act, because a seek invalidates every other type's state and only their owner may do that.

    // Throws away everything concluded since the last reposition, in every type that concluded
    // anything: the decoder's received frame, the selector's frames and frontier, the probe frame,
    // the index's contiguity and the reader's position.
    void resetPosition() noexcept
    {
        decoder.releaseFrame();
        selector.reset();
        probeFrame = nullptr;
        keyframes.resetContiguity();
        packetReader.resetPosition();
    }

    void afterSeek (std::int64_t target, bool atStart) noexcept
    {
        decoder.flush();
        resetPosition(); // nothing it drops is read between here and the mark below
        position.markSeeked (target, atStart);
        selector.setSynthOrigin (target); // after reset(), which clears it
    }

    // Seeks towards `target`. Where the seek actually landed is established by readLanding().
    [[nodiscard]] std::expected<void, Error> seekTo (std::int64_t target)
    {
        const int result = source.seekTo (target);

        if (result < 0)
        {
            // The demuxer may have moved; nothing held is trustworthy any more.
            resetPosition();
            position.markInvalid();
            return fail (ErrorCode::seekFailed, result, "avformat_seek_file");
        }

        afterSeek (target, false);
        return {};
    }

    // Performs MediaSource::byteSeekTo() for a recorded keyframe and records the landing at that
    // keyframe's presentation time.
    [[nodiscard]] std::expected<void, Error> byteSeek (const KeyEntry& entry)
    {
        const int result = source.byteSeekTo (entry.pos);

        if (result < 0)
        {
            resetPosition();
            position.markInvalid();
            return fail (ErrorCode::seekFailed, result, "av_seek_frame(AVSEEK_FLAG_BYTE)");
        }

        afterSeek (entry.pts, false);
        return {};
    }

    // Positions on the very first packet, unconditionally (how each demuxer resolves that is
    // MediaSource::seekToStart()'s comment). Adds the policy: a source that cannot seek at all is
    // re-opened instead, one that cannot even be re-opened fails with notSeekable, and a landing
    // here is the one that sets landedAtStart.
    [[nodiscard]] std::expected<void, Error> seekToStart()
    {
        if (! getStreamInfo().seekable)
        {
            if (! getStreamInfo().ioSeekable) return fail (ErrorCode::notSeekable, "the source cannot be rewound");
            return reopen();
        }

        const int result = source.seekToStart();

        if (result < 0)
        {
            resetPosition();
            position.markInvalid();

            if (result == libav::exitRequested)
            {
                return fail (ErrorCode::seekFailed, result, "avformat_seek_file(start)");
            }

            if (! getStreamInfo().ioSeekable)
            {
                return fail (ErrorCode::notSeekable, result,
                             "seeking to the start failed and the source cannot be re-opened");
            }

            return reopen();
        }

        afterSeek (getStreamInfo().startPts, true);
        return {};
    }

    // Re-establishes the source from scratch (non-seekable sources that must rewind, or after a
    // failed seek) and rebuilds the decoder on top of it.
    //
    // MediaSource::reopen() re-opens the container and returns: the container and the decoder have
    // different lifetimes (this one container outlives two or three decoder rebuilds on the hardware
    // fallback path), so the order of the two is decided here rather than buried in the re-open.
    // buildCodec() is what resets the decode position, exactly as it does for every other rebuild.
    //
    // No re-open under an already-cancelled request: the re-probe runs its I/O through the interrupt
    // callback and would keep only what it managed to read. Checked here rather than inside
    // MediaSource, because by the time the container is touched this has already given up its
    // decoder, and a cancelled request must not pay that.
    [[nodiscard]] std::expected<void, Error> reopen()
    {
        if (source.isCancelled())
        {
            return fail (ErrorCode::cancelled, libav::exitRequested, "cancelled before re-opening the source");
        }

        const std::optional<HwCandidate> candidate = decoder.getHardwareCandidate();
        decoder.releaseContext();
        position.markInvalid();
        auto result = source.reopen (options);

        if (result) result = attachToSource();
        if (result) result = buildCodec (candidate ? &*candidate : nullptr);
        if (! result)
        {
            // The old context is gone and the new one failed; imageAt() retries on the next request.
            // close() is idempotent: MediaSource::reopen() has already closed when it was the step that
            // failed, and has not when attachToSource() or buildCodec() was.
            decoder.releaseContext();
            source.close();
            brokenReason = result.error().message;
            return fail (ErrorCode::seekFailed, result.error().avError,
                         "re-opening the source: " + result.error().message);
        }

        selector.resetTailEnd(); // a re-opened (or grown) source may reach further than it did
        packetReader.resetVerifiedTo();
        position.markSeeked (getStreamInfo().startPts, /*atStart=*/true);
        return {};
    }

    // Throws away everything the decoder and this pipeline believed after an interrupt aborted a
    // libavformat read. MediaSource clears libavio's sticky state and flushes the demuxer; the
    // decoder flush, the frontier and the position are this pipeline's to drop.
    //
    // Returns false when the sticky state could not be cleared in place (see
    // MediaSource::recoverAfterInterrupt) and the source can be re-opened. The caller must then
    // re-establish it before the next read; nothing else clears the I/O layer.
    [[nodiscard]] bool recoverAfterInterrupt() noexcept
    {
        const MediaSource::IoState ioState = source.recoverAfterInterrupt();

        if (ioState == MediaSource::IoState::clean) return true;
        const std::int64_t lastReceived = selector.getLastReceivedTs();
        decoder.flush();
        resetPosition();

        if (ioState == MediaSource::IoState::stuck && getStreamInfo().ioSeekable)
        {
            position.markInvalid(); // nothing here can unstick the I/O layer; the caller re-opens
            return false;
        }

        if (getStreamInfo().seekable)
        {
            position.markInvalid();
        }
        else
        {
            // Best effort on a non-rewindable input: the demuxer is somewhere at or after the last frame.
            // The one field carried across the reset: the rest of the frontier described a decoder state
            // the flush above destroyed, but this one is still true of the source.
            position.markCarriedForward();
            selector.carryForwardLastReceivedTs (lastReceived);
        }

        return true;
    }

    // Re-positions for `target` after an end of stream that produced no frames at all. libavio's
    // sticky end-of-file is cleared first: a seek leaves it set, so the retry would read EOF again.
    // On a libavformat major where it cannot be cleared in place, the source is re-opened instead.
    [[nodiscard]] std::expected<void, Error> retryAfterEmptyEof (std::int64_t target, const CancelToken& token)
    {
        if (! source.tryResetIo() && getStreamInfo().ioSeekable)
        {
            resetPosition();

            if (auto result = reopen(); ! result) return result;
            return positionFor (target, selector.isKeyframeOnly(), token);
        }

        source.flushDemuxer();
        decoder.flush();
        resetPosition();
        position.markInvalid(); // a real seek, not a decode-forward from a position that read EOF
        return positionFor (target, selector.isKeyframeOnly(), token);
    }

    // Seeks towards `target`, with the failure policy: interrupted I/O is reported as is, other
    // failures count towards giving up on seeking and fall back to a re-open.
    [[nodiscard]] std::expected<void, Error> seekOrReopen (std::int64_t target)
    {
        if (target <= getStreamInfo().startPts) return seekToStart();
        auto result = seekTo (target);

        if (result)
        {
            source.noteSeekSucceeded();
            return {};
        }

        if (result.error().avError == libav::exitRequested) return result;
        source.noteSeekFailed();

        if (! getStreamInfo().ioSeekable)
        {
            return fail (ErrorCode::notSeekable, result.error().avError,
                         "seeking failed and the source cannot be re-opened");
        }

        return reopen();
    }

    // Decides between continuing to decode forward and seeking, then positions accordingly. In
    // keyframe mode the answer is a keyframe packet that is then decoded alone
    // (armKeyframeDecode).
    [[nodiscard]] std::expected<void, Error> positionFor (std::int64_t target, bool keyframeMode,
                                                          const CancelToken& token)
    {
        positioner.resetBackoff();

        if (! getStreamInfo().seekable)
        {
            if (Positioner::canDecodeForwardTo (target, makePositioningView())) return {};
            if (Positioner::isUndecodedStartLanding (makePositioningView())) return {};
            if (! getStreamInfo().ioSeekable)
            {
                return fail (ErrorCode::notSeekable,
                             "the source is not seekable; requested times must not precede the current position");
            }

            return reopen();
        }

        if (! keyframeMode)
        {
            if (Positioner::canDecodeForwardTo (target, makePositioningView())
                && positioner.isForwardCheaperThanSeek (target, makePositioningView()))
            {
                return {};
            }

            // Freshly positioned just before the target: select() verifies the landing against the
            // first frame out.
            if (positioner.isUndecodedLandingBefore (target, makePositioningView())) return {};
        }

        if (getStreamInfo().indexTrusted) return positionIndexed (target, keyframeMode, token);
        return positionScanned (target, keyframeMode, token);
    }

    // Containers with a trusted index (MP4, Matroska): seek to the target, read the landing
    // keyframe packet, and correct an overshoot (fragmented MP4, open-GOP leading pictures) with a
    // re-seek to the previous keyframe. Where each of those aims is Positioner's; this performs
    // them and reads the landings in between.
    [[nodiscard]] std::expected<void, Error> positionIndexed (std::int64_t target, bool keyframeMode,
                                                              const CancelToken& token)
    {
        std::int64_t seekAim = positioner.getIndexedAim (target);
        bool needSeek = true;

        for (int round = 0;; ++round)
        {
            if (needSeek)
            {
                if (auto result = seekOrReopen (seekAim); ! result) return result;
            }

            needSeek = true;

            if (! getStreamInfo().seekable) return {}; // gave up on seeking: the re-open positioned at the start
            auto landing = readLanding (target, /*scan=*/false, keyframeMode, token);

            if (! landing) return std::unexpected (std::move (landing.error()));
            if (landing->found || position.isLandedAtStart() || ! packetReader.areKeyFlagsReliable()) return {};
            if (landing->firstKeyPts == libav::noPts)
            {
                // No keyframe packet before the end: the seek landed in the tail. backOff() seeks itself.
                if (auto moved = backOff (target, libav::noPts); ! moved) return moved;
                needSeek = false;
                continue;
            }

            // Overshoot: the landing keyframe is past the target. One cheap seek to the keyframe strictly
            // before it, or the start once the rounds are spent.
            const Positioner::Aim retry = positioner.retryAfterOvershoot (
                target, landing->firstKeyPts, landing->firstKeyDts, round, makePositioningView());

            if (retry.toStart) return seekToStartAndLand (target, keyframeMode, token);
            seekAim = retry.target;
        }
    }

    [[nodiscard]] std::expected<void, Error> seekToStartAndLand (std::int64_t target, bool keyframeMode,
                                                                 const CancelToken& token)
    {
        if (auto result = seekToStart(); ! result) return result;
        auto landing = readLanding (target, /*scan=*/! getStreamInfo().indexTrusted, keyframeMode, token);

        if (! landing) return std::unexpected (std::move (landing.error()));
        return {};
    }

    // Containers without a trusted index (MPEG-TS): position with a byte seek when the keyframe
    // covering the target is already known; otherwise aim one GOP early, scan the packets up to the
    // target and choose the last keyframe at or before it (recording every keyframe on the way).
    [[nodiscard]] std::expected<void, Error> positionScanned (std::int64_t target, bool keyframeMode,
                                                              const CancelToken& token)
    {
        if (const KeyEntry* entry = keyframes.findCoveringKey (target); entry != nullptr)
        {
            if (auto result = byteSeek (*entry); ! result) return result;
            if (! keyframeMode) return {};
            auto landing = readLanding (target, /*scan=*/false, keyframeMode, token);

            if (! landing) return std::unexpected (std::move (landing.error()));
            if (landing->found) return {};
            // The recorded position no longer holds (the file changed): fall through to a fresh scan.
        }

        if (auto result = seekOrReopen (Positioner::getScanAim (target, makePositioningView())); ! result)
        {
            return result;
        }

        for (;;)
        {
            if (! getStreamInfo().seekable) return {};
            auto landing = readLanding (target, /*scan=*/true, keyframeMode, token);

            if (! landing) return std::unexpected (std::move (landing.error()));
            if (landing->found || position.isLandedAtStart() || ! packetReader.areKeyFlagsReliable()) return {};
            // Landed after the keyframe covering the target (or in the tail): retreat by a growing step.
            if (auto moved = backOff (target, landing->firstKeyPts); ! moved) return moved;
        }
    }

    // Retreats after a landing proved to be past `target`: Positioner picks the new aim (or gives
    // up and sends us to the start), this performs the seek it asked for.
    [[nodiscard]] std::expected<void, Error> backOff (std::int64_t target,
                                                      std::int64_t observedKey /* or libav::noPts */)
    {
        const Positioner::Aim aim = positioner.nextBackoff (target, observedKey, makePositioningView());

        if (aim.toStart) return seekToStart();
        return seekOrReopen (aim.target);
    }

    // PacketReader::readLanding() plus the part only positioning may carry out. The scan reads
    // packets and chooses the keyframe; going back to one is a seek, and seeks are not the
    // reader's. Returned as data rather than done in place so the dependency runs one way:
    // positioning drives the reader, never the other way round.
    [[nodiscard]] std::expected<PacketReader::Landing, Error> readLanding (std::int64_t target, bool scan,
                                                                           bool keyframeMode, const CancelToken& token)
    {
        // The frontier's taint is the selector's to own; the reader reports a hole in what it read
        // through the same flag, so it is lent for the duration of the read.
        bool tainted = selector.getFrontier().tainted;
        auto landing = packetReader.readLanding (source, keyframes, target, scan, keyframeMode, tainted, token);

        if (tainted) selector.noteDemuxTaint();
        if (! landing) return landing;
        if (landing->awaitKey) position.markAwaitingKey();
        switch (landing->rewind)
        {
        case PacketReader::Landing::Rewind::none:
            break;
        case PacketReader::Landing::Rewind::byteSeek:
            if (auto result = byteSeek (landing->rewindEntry); ! result)
                return std::unexpected (std::move (result.error()));
            break;
        case PacketReader::Landing::Rewind::timestampSeek:
            if (auto result = seekOrReopen (landing->rewindTs); ! result)
                return std::unexpected (std::move (result.error()));
            break;
        }

        return landing;
    }

    // ---- output ------------------------------------------------------------------------------

    [[nodiscard]] std::expected<Image, Error> convert (Selected selected)
    {
        const AVFrame* sourceFrame = selected.frame;
        FramePtr softwareFrame;

        if (sourceFrame->hw_frames_ctx != nullptr)
        {
            auto downloaded = converter->download (*sourceFrame);

            if (! downloaded)
            {
                decoder.noteHardwareFault();
                return std::unexpected (std::move (downloaded.error()));
            }

            softwareFrame = std::move (*downloaded);
            sourceFrame = softwareFrame.get();
        }

        auto converted = converter->convert (*sourceFrame, getStreamInfo().containerSar, getStreamInfo().codecSar,
                                             requestedMaximumSize);

        if (! converted) return std::unexpected (std::move (converted.error()));
        // A frame before the time origin (MPEG-TS whose start comes from another stream) is reported at
        // zero.
        const std::int64_t relativeTs = std::max<std::int64_t> (
            selector.getFrameTs (*selected.frame, getStreamInfo()) - getStreamInfo().startPts, 0);
        const Rational timeBase = fromAv (getStreamInfo().timeBase);
        const Time actual = Time::fromTimestamp (relativeTs, timeBase);
        const bool isKey = (selected.frame->flags & AV_FRAME_FLAG_KEY) != 0;
        const ColorRange colorRange = fromAv (static_cast<AVColorRange> ((*converted)->color_range));
        const std::int64_t duration = FrameSelector::getFrameDuration (*selected.frame, getStreamInfo());
        (*converted)->duration = duration;
        Image image =
            ImageAccess::make (std::move (*converted), options.pixelFormat, colorRange,
                               actual.isValid() ? actual : Time::zero(), isKey, selected.adjustment, selected.corrupt);

        if (duration > 0) ImageAccess::setDuration (image, Time::fromTimestamp (duration, timeBase));
        return image;
    }

    MediaSource source;
    Options options;
    VideoDecoder decoder;
    AssetInfo info;
    std::optional<Converter> converter;
    KeyframeIndex keyframes; // where the keyframes are; the reader records into it as it reads
    PacketReader packetReader;
    Positioner positioner;  // decides where to seek; the seeking itself is done here
    FrameSelector selector; // decides which frame answers; the repositioning it asks for is done here
    Position position;      // where the demuxer is; written only through its own verbs
    SeekCostModel costs;    // what a seek and a frame cost; drives isForwardCheaperThanSeek()
    const AVFrame* probeFrame{ nullptr };
    std::int64_t firstFrameTs{ libav::noPts };
    std::optional<Size> requestedMaximumSize; // RequestOptions::maximumSize of the current request
    RequestStats requestStats;
    // Why this pipeline is currently unusable: a re-open that failed, or a software decoder that
    // could not be rebuilt after a hardware failure. Either being dead is what makes the pipeline
    // dead, and the one reader (selectFor's "the generator is unusable") tests both. The pipeline
    // retries on the next request.
    std::string brokenReason;
};

} // namespace stills::detail

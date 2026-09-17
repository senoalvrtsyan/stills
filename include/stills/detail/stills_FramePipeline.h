#pragma once
// stills/detail/stills_FramePipeline.h — the decoder and everything built on it: hardware setup with
// software fallback, accurate seeking, the decode loop, frame selection, and conversion to the
// output Image.
//
// The container and its I/O live in MediaSource (detail/stills_MediaSource.h), which this owns and
// drives. Anything reaching the AVFormatContext goes through it. The packets themselves, the GOP
// replay buffer and the landing scan live in PacketReader (detail/stills_PacketReader.h); where the
// keyframes are lives in KeyframeIndex (detail/stills_KeyframeIndex.h); the codec context, the
// hardware session and the frame the decoder writes into live in VideoDecoder
// (detail/stills_VideoDecoder.h); and where to seek next is decided by Positioner
// (detail/stills_Positioner.h), which never seeks -- the seeking, and the invalidation every seek
// implies, is what is left here. All five are owned here and driven here, and none of them knows
// about this class or about each other's owner.
//
// A FramePipeline is single-threaded by contract: callers serialise access (see stills_Engine.h).

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

#include "stills/detail/stills_Converter.h"
#include "stills/detail/stills_DecodeFrontier.h"
#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_FrameSlot.h"
#include "stills/detail/stills_HardwareSupport.h"
#include "stills/detail/stills_KeyframeIndex.h"
#include "stills/detail/stills_MediaSource.h"
#include "stills/detail/stills_PacketReader.h"
#include "stills/detail/stills_Positioner.h"
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
    /// A frame chosen for a request, owned by the pipeline (one of its FrameSlots).
    struct Selected
    {
        AVFrame* frame{ nullptr };
        Adjustment adjustment{ Adjustment::none };
        bool corrupt{ false };
    };

    FramePipeline (const FramePipeline&) = delete;
    FramePipeline& operator= (const FramePipeline&) = delete;
    FramePipeline (FramePipeline&&) = delete;
    FramePipeline& operator= (FramePipeline&&) = delete;
    /// Opens the source. `token` (optional) makes the open cancellable: libavformat's interrupt
    /// callback consults it during probing and the first decode, so a stalled network source fails
    /// with `cancelled` / `openFailed` instead of blocking.
    [[nodiscard]] static std::expected<std::unique_ptr<FramePipeline>, Error> open (std::string source, Options options,
                                                                                    const CancelToken* token = nullptr)
    {
        std::unique_ptr<FramePipeline> p{ new FramePipeline (std::move (source), std::move (options)) };
        p->source.setCancelToken (token);
        auto finish = [&] (Error e) -> std::expected<std::unique_ptr<FramePipeline>, Error>
        {
            p->source.setCancelToken (nullptr);

            if (token != nullptr && token->isRequested()) return fail (ErrorCode::cancelled, "open cancelled");
            return std::unexpected (std::move (e));
        };

        if (auto r = p->source.open (p->opt); ! r) return finish (std::move (r.error()));
        if (auto r = p->attachToSource(); ! r) return finish (std::move (r.error()));
        if (auto r = p->setupDecoder(); ! r) return finish (std::move (r.error()));
        p->fillInfo();
        p->source.setCancelToken (nullptr);
        return p;
    }

    [[nodiscard]] const AssetInfo& getInfo() const noexcept { return info; }

    /// Snapshot of the active decode path. Returned by value: a lazy hardware failure may switch
    /// the path to software after open(), and readers must never observe a torn value. The only
    /// member function of this class another thread may call (see stills_VideoDecoder.h).
    [[nodiscard]] ActiveDecoder getActiveDecoder() const { return decoder.getActiveDecoder(); }
    [[nodiscard]] const Options& options() const noexcept { return opt; }

    // Seeks and decoded frames attributable to the request that just returned: cur is reset at the
    // top of imageAt() and nowhere else. Both counters are maintained for SeekCostModel and
    // isForwardCheaperThanSeek(), so reading them adds no work to the decode path and a build that
    // never calls these pays nothing. The benchmark harness (tests/bench) uses them to check that a
    // restructure changes neither count -- which seeks happen is the behaviour, the milliseconds are
    // only the machine.
    //
    // Both are differences rather than counters of their own: MediaSource counts every seek it
    // issues and VideoDecoder every frame it returns, because that is where the calls are made and a
    // count kept anywhere else could drift out of step with them, and a request is not a scope
    // either of them knows about.
    [[nodiscard]] int getSeekCount() const noexcept
    {
        return static_cast<int> (source.getSeekCallCount() - cur.seeksAtStart);
    }

    [[nodiscard]] int getDecodedFrameCount() const noexcept
    {
        return static_cast<int> (decoder.getReceiveCount() - cur.framesAtStart);
    }

    /// Extracts the frame for `requested` (asset-relative). Thread-unsafe by design.
    [[nodiscard]] std::expected<Image, Error> imageAt (Time requested, const CancelToken& token)
    {
        return imageAt (requested, RequestOptions{}, token);
    }

    /// The decode path allocates freely and the async worker's loop is noexcept, so a std::bad_alloc
    /// below here would terminate instead of surfacing the outOfMemory the API defines. Caught at
    /// this frame, the only one that can drop the half-updated state: the next request re-positions.
    [[nodiscard]] std::expected<Image, Error> imageAt (Time requested, const RequestOptions& ro,
                                                       const CancelToken& token)
    {
        cur = RequestStats{ .seeksAtStart = source.getSeekCallCount(), .framesAtStart = decoder.getReceiveCount() };
        try
        {
            return imageAtImpl (requested, ro, token);
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
    [[nodiscard]] std::expected<Image, Error> imageAtImpl (Time requested, const RequestOptions& ro,
                                                           const CancelToken& token)
    {
        source.setCancelToken (&token);
        // Only a fault raised by *this* request may drive the rebuild below. One left over from a
        // candidate probed at open would otherwise turn the next unrelated failure (a cancellation, a
        // timeOutOfRange) into a decoder rebuild and rewrite fallbackReason.
        decoder.clearHardwareFault();
        const auto attempt = [&]() -> std::expected<Image, Error>
        {
            auto sel = selectFor (requested, ro, token);

            if (! sel) return std::unexpected (std::move (sel.error()));
            return convert (*sel);
        };

        auto result = attempt();

        if (! result && decoder.hasHardwareFault() && decoder.isHardwareActive() && ! decoder.hasRetriedHardware())
        {
            // Hardware faults after open (an EIO on a surface download, a lost session) are usually
            // transient and clear on a fresh session, so rebuild the same hardware decoder once before
            // falling back to software.
            decoder.clearHardwareFault();
            decoder.noteHardwareRetry();
            const std::string why = result.error().message;

            if (auto r = rebuildHardware(); r)
            {
                result = attempt();
            }
            else
            {
                decoder.noteHardwareFault(); // fall through to the software fallback below
                result = std::unexpected (
                    makeError (ErrorCode::decodeFailed, 0, why + "; hardware rebuild failed: " + r.error().message));
            }
        }

        if (! result && decoder.hasHardwareFault())
        {
            decoder.clearHardwareFault();

            if (opt.hardware.policy == HardwarePolicy::requireHardware)
            {
                source.setCancelToken (nullptr);
                return std::unexpected (makeError (ErrorCode::hardwareUnavailable, 0,
                                                   "hardware decoding failed: " + result.error().message));
            }

            if (auto r = rebuildSoftware ("hardware decoder failed during decoding: " + result.error().message); ! r)
            {
                source.setCancelToken (nullptr);
                return std::unexpected (std::move (r.error()));
            }

            result = attempt();
        }

        if (result && decoder.isHardwareActive())
            decoder.rearmHardwareRetry(); // a successful hardware request re-arms the retry
        source.setCancelToken (nullptr);

        // AVERROR_EXIT with a cancelled token is the cancellation; with a live token it is a stale
        // interrupt and stays an I/O failure.
        if (! result && result.error().avError == k::exitRequested && token.isRequested())
        {
            return fail (ErrorCode::cancelled, "cancelled");
        }

        return result;
    }

    /// Positions and selects the frame for `requested` without converting it (the frame stays owned
    /// by the pipeline, in one of its FrameSlots). Validates the request, maps the time into stream
    /// ticks, applies the bounds policy and the tolerance window.
    [[nodiscard]] std::expected<Selected, Error> selectFor (Time requested, const RequestOptions& ro,
                                                            const CancelToken& token)
    {
        if (auto v = ro.validate (opt.pixelFormat); ! v) return std::unexpected (std::move (v.error()));
        reqMax = ro.maximumSize;
        const Tolerance tolerance = ro.tolerance.value_or (opt.tolerance);

        if (! requested.isFinite())
        {
            return fail (ErrorCode::invalidArgument, "requested time is not finite: " + toString (requested));
        }

        if (requested.isNegative())
        {
            return fail (ErrorCode::invalidArgument, "requested time is negative: " + toString (requested));
        }

        // Nearest tick, not floor: coarse time bases (Matroska/WebM: 1 ms) store k/fps rounded to the
        // nearest tick, so an exact k/fps lands a fraction of a tick below frame k and flooring would
        // return frame k-1.
        const std::int64_t rel = requested.toTimestamp (fromAv (getStreamInfo().timeBase), TimeRounding::nearest);

        if (rel == std::numeric_limits<std::int64_t>::max())
        {
            return fail (ErrorCode::timeOutOfRange, "requested time does not fit the stream time base");
        }

        std::int64_t target = 0;

        if (__builtin_add_overflow (getStreamInfo().startPts, rel, &target))
        {
            // A representable Time can still fall outside the stream's timestamp domain once the origin
            // is added (an MPEG-TS starting at 10 s). Report it rather than wrapping.
            return fail (ErrorCode::timeOutOfRange, "requested time does not fit the stream time base");
        }

        Adjustment clamped = Adjustment::none;

        // Containers occasionally understate their duration by a frame; give one frame of slack before
        // rejecting up front, and let the decoder (EOF) decide inside that margin.
        if (getStreamInfo().durationPts
            && rel > *getStreamInfo().durationPts + std::max<std::int64_t> (getStreamInfo().frameDurationHint, 0))
        {
            if (opt.outOfRange == OutOfRangePolicy::error)
            {
                return fail (ErrorCode::timeOutOfRange,
                             toString (requested) + " is beyond the asset duration "
                                 + toString (Time::fromTimestamp (*getStreamInfo().durationPts,
                                                                  fromAv (getStreamInfo().timeBase))));
            }

            target = getStreamInfo().startPts + *getStreamInfo().durationPts;
            clamped = Adjustment::clampedToLast;
        }

        // Tolerance window in stream ticks (validate() rejects negative tolerances).
        const auto windowEdge = [&] (Time tol, bool isBefore) -> std::int64_t
        {
            if (tol.isPositiveInfinity())
            {
                return isBefore ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
            }

            std::int64_t ticks = 0;

            if (tol.isFinite() && tol > Time::zero())
            {
                ticks = tol.toTimestamp (fromAv (getStreamInfo().timeBase), TimeRounding::down);
            }

            // A tolerance that reaches beyond half the timestamp domain is treated as an infinite one.
            // The edge is formed first and compared afterwards: `ticks > target - INT64_MIN / 2` is the
            // same inequality, but forms `target - INT64_MIN / 2` even when the tolerance is zero, which
            // overflows for a target past INT64_MAX / 2 — reachable from imageAt() on a source with no
            // declared duration, where the bounds check above cannot reject the request first.
            std::int64_t edge = 0;

            if (isBefore)
            {
                if (__builtin_sub_overflow (target, ticks, &edge)
                    || edge < std::numeric_limits<std::int64_t>::min() / 2)
                {
                    return std::numeric_limits<std::int64_t>::min();
                }

                return edge;
            }

            if (__builtin_add_overflow (target, ticks, &edge) || edge > std::numeric_limits<std::int64_t>::max() / 2)
            {
                return std::numeric_limits<std::int64_t>::max();
            }

            return edge;
        };

        const std::int64_t lo = windowEdge (tolerance.before, true);
        const std::int64_t hi = windowEdge (tolerance.after, false);

        if (! source.isOpen() || ! decoder.isBuilt())
        {
            // A previous re-open failed (or was cancelled): retry rather than staying dead forever.
            if (auto r = reopen(); ! r)
            {
                return fail (ErrorCode::unusable, r.error().avError, "the generator is unusable: " + brokenReason);
            }
        }

        if (! recoverAfterInterrupt())
        {
            // The interrupt left libavio's state stuck and this libavformat major is not one
            // MediaSource::tryResetIoState() can clear: re-open instead of reading through it.
            if (auto r = reopen(); ! r) return std::unexpected (std::move (r.error()));
        }

        eofRetried = false; // one re-position per attempt (a hardware fallback retries the request)
        return extract (target, lo, hi, clamped, token);
    }

    static constexpr int maxConsecutiveErrors = 32;

    FramePipeline (std::string source, Options options) : source (std::move (source)), opt (std::move (options)) {}

    // What the source learned about the chosen video stream. MediaSource is the only writer; the
    // handful of fields the pipeline has to change (the time origin, the decoder's frame rate,
    // seekability after a run of failed seeks) go back through named MediaSource methods, so no
    // caller can set one behind its back.
    [[nodiscard]] const StreamInfo& getStreamInfo() const noexcept { return source.getStreamInfo(); }

public:
    ~FramePipeline() = default;

private:
    /// The pipeline's own half of opening: the decoder libavformat picked for the stream, the packet
    /// and frame buffers, and the converter (whose orientation comes from the stream's display
    /// matrix). Runs after every MediaSource open and re-open, and owns nothing MediaSource owns.
    [[nodiscard]] std::expected<void, Error> attachToSource()
    {
        if (auto r = decoder.attach (source.getCodec()); ! r) return r;
        if (auto r = packets.attach(); ! r) return r;
        for (FrameSlot* slot : { &held, &pending, &corruptLast })
        {
            auto fr2 = makeFrame();

            if (! fr2) return std::unexpected (fr2.error());
            slot->install (std::move (*fr2));
        }

        const DisplayTransform applied =
            opt.applyPreferredTrackTransform ? getStreamInfo().transform : DisplayTransform{};
        converter.emplace (opt.pixelFormat, opt.scaler, opt.maximumSize, opt.applySampleAspectRatio, applied.rotation,
                           applied.mirrored,
                           /*stripDisplayMatrix=*/opt.applyPreferredTrackTransform);
        return {};
    }

    /// Builds (or replaces) the decoder for the stream, and puts the pipeline back to knowing
    /// nothing: a frame decoded by the decoder being replaced says nothing about where its
    /// replacement is. The drop happens before the build rather than inside it, so the surfaces a
    /// hardware decoder handed us are given back before its pool is.
    [[nodiscard]] std::expected<void, Error> buildCodec (const HwCandidate* hw)
    {
        resetPosition(); // including the recorded holes: a new decoder produces them all again
        const AVStream* st = source.getStream();

        if (auto r = decoder.build (*st->codecpar, st->time_base, opt, hw); ! r) return r;
        // Raw elementary streams carry no timestamps of their own, so the frame rate the decoder
        // negotiated is the only one there is. The decoder reports it; what to do with it is the
        // source's, which is why it comes back as a value rather than being written from inside.
        if (getStreamInfo().synthesizeTimestamps)
        {
            const AVRational fr = decoder.getFrameRate();

            if (fr.num > 0 && fr.den > 0) source.adoptDecoderFrameRate (fr);
            if (getStreamInfo().frameDurationHint <= 0)
            {
                source.adoptDecoderFrameRate (AVRational{ 25, 1 }); // libav's own default
            }
        }

        return {};
    }

    /// Tries the hardware candidates in preference order, then software. Each attempt is validated
    /// by decoding the first frame so lazy failures surface here, not on the first request.
    [[nodiscard]] std::expected<void, Error> setupDecoder()
    {
        std::string reason;
        Options effective = opt;

        if (effective.hardware.policy == HardwarePolicy::automatic)
        {
            effective.hardware.policy =
                VideoDecoder::isHardwareWorthwhile (*source.getStream()->codecpar, *source.getCodec())
                    ? HardwarePolicy::preferHardware
                    : HardwarePolicy::softwareOnly;
        }

        std::vector<HwCandidate> candidates = hwCandidates (source.getCodec(), effective, reason);

        if (opt.hardware.policy == HardwarePolicy::automatic
            && effective.hardware.policy == HardwarePolicy::softwareOnly)
        {
            const AVCodecParameters& par = *source.getStream()->codecpar;
            reason = "automatic policy: software decoding is faster for " + std::to_string (par.width) + "x"
                     + std::to_string (par.height) + " " + decoder.getCodecName();
        }

        if (! candidates.empty() && ! getStreamInfo().ioSeekable)
        {
            // Probing a candidate decodes the first frame and a rejected one needs the input rewound;
            // a pipe cannot be rewound, so the probe would consume it. Software needs exactly one pass.
            candidates.clear();
            reason = "non-rewindable input: hardware probing would consume it";
        }

        std::string attempts;

        for (const HwCandidate& c : candidates)
        {
            auto built = buildCodec (&c);
            std::expected<void, Error> probed = built ? probeFirstFrame() : std::expected<void, Error>{};

            if (built && probed && decoder.didAcceptHardwareFormat())
            {
                decoder.publishHardware (opt.hardware.device);
                return {};
            }

            const std::string why =
                ! built    ? built.error().message
                : ! probed ? probed.error().message
                           : std::string ("decoder declined the hardware pixel format (profile not supported)");

            if (! attempts.empty()) attempts += "; ";
            attempts += VideoDecoder::getHardwareTypeName (c.type) + ": " + why;
        }

        if (opt.hardware.policy == HardwarePolicy::requireHardware)
        {
            return fail (ErrorCode::hardwareUnavailable,
                         "no hardware decoder could be used (" + (attempts.empty() ? reason : attempts) + ")");
        }

        if (auto r = buildCodec (nullptr); ! r) return r;
        if (auto r = probeFirstFrame(); ! r) return r;
        decoder.publishSoftware (attempts.empty() ? reason : attempts);
        return {};
    }

    /// Re-creates the active hardware decoder (same device type and format) and re-probes it.
    [[nodiscard]] std::expected<void, Error> rebuildHardware()
    {
        const std::optional<HwCandidate> c = decoder.getHardwareCandidate();

        if (! c) return fail (ErrorCode::internal, "no hardware decoder to rebuild");
        if (auto r = buildCodec (&*c); ! r) return r;
        if (auto p = probeFirstFrame(); ! p) return p;
        if (! decoder.didAcceptHardwareFormat())
        {
            return fail (ErrorCode::hardwareUnavailable, "rebuilt decoder declined the hardware format");
        }

        return {};
    }

    [[nodiscard]] std::expected<void, Error> rebuildSoftware (std::string reason)
    {
        auto r = buildCodec (nullptr);

        if (! r)
        {
            brokenReason = "software decoder could not be rebuilt after a hardware failure: " + r.error().message;
            return r;
        }

        // Report the software decoder even if the probe below fails: an I/O error there leaves a
        // working software path whose first request simply failed.
        decoder.publishSoftware (reason);

        if (auto p = probeFirstFrame(); ! p)
        {
            decoder.publishSoftware (std::move (reason) + "; probe after rebuild failed: " + p.error().message);
            return p;
        }

        return {};
    }

    /// Decodes the first frame of the stream (any tolerance) and remembers its timestamp. Always
    /// goes through the explicit start seek, so on containers whose seek does not land on
    /// keyframes (MPEG-TS) the "first frame" really is the first frame, for every candidate.
    [[nodiscard]] std::expected<void, Error> probeFirstFrame()
    {
        CancelToken none;
        keyframeOnly = false;

        if (position.isPositioned())
        {
            if (auto r = seekToStart(); ! r) return r;
        }
        else
        {
            position.markAtStart (getStreamInfo().startPts);
        }

        auto sel = select (getStreamInfo().startPts, std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max(), none);

        if (! sel)
        {
            if (decoder.isHardwareActive())
                return fail (ErrorCode::hardwareUnavailable, "first frame: " + sel.error().message);
            return std::unexpected (std::move (sel.error()));
        }

        if (decoder.isHardwareActive() && sel->frame->format != decoder.getHardwarePixelFormat())
        {
            return fail (ErrorCode::hardwareUnavailable, "decoder produced a software frame");
        }

        firstFrameTs = getFrameTs (*sel->frame);
        probeFrame = sel->frame;

        // Raw streams: anchor Time::zero() at the first frame.
        if (source.needsStartPtsFromFirstFrame()) source.setStartPts (firstFrameTs);
        return {};
    }

    [[nodiscard]] AVRational getEffectiveSar() const noexcept
    {
        if (getStreamInfo().containerSar.num > 0 && getStreamInfo().containerSar.den > 0)
            return getStreamInfo().containerSar;

        if (probeFrame != nullptr && probeFrame->sample_aspect_ratio.num > 0 && probeFrame->sample_aspect_ratio.den > 0)
        {
            return probeFrame->sample_aspect_ratio;
        }

        if (getStreamInfo().codecSar.num > 0 && getStreamInfo().codecSar.den > 0) return getStreamInfo().codecSar;
        return AVRational{ 1, 1 };
    }

    void fillInfo()
    {
        info = AssetInfo{};
        info.containerName = source.getContainerName();
        info.codecName = decoder.getCodecName();
        const auto srcFmt = static_cast<AVPixelFormat> (source.getStream()->codecpar->format);
        const char* fmtName = av_get_pix_fmt_name (srcFmt);

        if (fmtName == nullptr && probeFrame != nullptr)
        {
            fmtName = av_get_pix_fmt_name (static_cast<AVPixelFormat> (probeFrame->format));
        }

        info.sourcePixelFormat = fmtName != nullptr ? fmtName : "";
        info.videoStreamIndex = getStreamInfo().index;
        info.timeBase = fromAv (getStreamInfo().timeBase);
        info.averageFrameRate =
            getStreamInfo().avgFrameRate.num > 0 ? fromAv (getStreamInfo().avgFrameRate) : Rational{ 0, 1 };

        if (getStreamInfo().durationPts)
        {
            info.duration = Time::fromTimestamp (*getStreamInfo().durationPts, info.timeBase);
        }

        if (source.getStream()->nb_frames > 0) info.frameCount = source.getStream()->nb_frames;
        Size coded{ source.getStream()->codecpar->width, source.getStream()->codecpar->height };

        if (probeFrame != nullptr && probeFrame->width > 0) coded = Size{ probeFrame->width, probeFrame->height };
        info.codedSize = coded;
        const AVRational sar = getEffectiveSar();
        const int rot = opt.applyPreferredTrackTransform ? getStreamInfo().transform.rotation : 0;
        info.displaySize = displaySize (coded, sar, opt.applySampleAspectRatio, rot);
        info.outputSize = converter->outputSize (coded, sar);
        info.rotationDegrees = getStreamInfo().transform.rotation;
        info.mirrored = getStreamInfo().transform.mirrored;
        info.sampleAspectRatio = fromAv (sar);
        info.seekable = getStreamInfo().seekable;
        info.timestampsSynthesized = getStreamInfo().synthesizeTimestamps;
        const auto nameOrEmpty = [] (const char* n) { return n != nullptr ? std::string{ n } : std::string{}; };
        const AVCodecParameters& par = *source.getStream()->codecpar;
        info.colorTransfer = par.color_trc != AVCOL_TRC_UNSPECIFIED
                                 ? nameOrEmpty (av_color_transfer_name (par.color_trc))
                                 : std::string{};
        info.colorPrimaries = par.color_primaries != AVCOL_PRI_UNSPECIFIED
                                  ? nameOrEmpty (av_color_primaries_name (par.color_primaries))
                                  : std::string{};
        info.colorSpace = par.color_space != AVCOL_SPC_UNSPECIFIED ? nameOrEmpty (av_color_space_name (par.color_space))
                                                                   : std::string{};
    }

    // Positioning. A seek is a hint: where it landed is established from the first keyframe packet
    // after it, before anything is decoded. Containers with a trusted index (MP4, Matroska) correct
    // an overshoot with one re-seek to the previous keyframe; containers without one (MPEG-TS) aim a
    // GOP early and scan packets up to the target, recording every keyframe (time, byte position, GOP
    // extent) in a lazily built index that later requests position from with one byte seek. Decoding
    // verifies the landing again, for demuxers whose keyframe flags cannot be trusted.

    void resetPosition() noexcept
    {
        decoder.releaseFrame();
        held.clear();
        pending.clear();
        corruptLast.clear();
        frontier.reset(); // including the recorded holes: nothing before a reposition can matter
        position.clearLanding();
        decodeErrors = 0;
        probeFrame = nullptr;
        keys.resetContiguity();
        packets.resetPosition();
    }

    void afterSeek (std::int64_t target, bool atStart) noexcept
    {
        decoder.flush();
        resetPosition(); // clears the landing; nothing it drops is read between here and the mark
        position.markSeeked (target, atStart);
        frontier.synthTs = target; // after resetPosition(), which clears it
    }

    /// Seeks towards `target`. Where the seek actually landed is established by readLanding().
    [[nodiscard]] std::expected<void, Error> seekTo (std::int64_t target)
    {
        const int r = source.seekTo (target);

        if (r < 0)
        {
            // The demuxer may have moved; nothing held is trustworthy any more.
            resetPosition();
            position.markInvalid();
            return fail (ErrorCode::seekFailed, r, "avformat_seek_file");
        }

        afterSeek (target, false);
        return {};
    }

    /// Positions on a recorded keyframe packet by byte offset (containers without a trusted index).
    /// The next packet read is that keyframe.
    [[nodiscard]] std::expected<void, Error> byteSeek (const KeyEntry& e)
    {
        const int r = source.byteSeekTo (e.pos);

        if (r < 0)
        {
            resetPosition();
            position.markInvalid();
            return fail (ErrorCode::seekFailed, r, "av_seek_frame(AVSEEK_FLAG_BYTE)");
        }

        afterSeek (e.pts, false);
        return {};
    }

    /// Positions on the very first packet, unconditionally. A timestamp below the stream's first
    /// DTS resolves to the first packet in libavformat's binary search (MPEG-TS/PS), to sample 0 in
    /// mov and to the first cue in Matroska; index-based demuxers that reject an out-of-range
    /// timestamp get a plain seek to startPts, and if that fails too the input is re-opened.
    [[nodiscard]] std::expected<void, Error> seekToStart()
    {
        if (! getStreamInfo().seekable)
        {
            if (! getStreamInfo().ioSeekable)
            {
                return fail (ErrorCode::notSeekable, "the source cannot be rewound");
            }

            return reopen();
        }

        const int r = source.seekToStart();

        if (r < 0)
        {
            resetPosition();
            position.markInvalid();

            if (r == k::exitRequested) return fail (ErrorCode::seekFailed, r, "avformat_seek_file(start)");
            if (! getStreamInfo().ioSeekable)
            {
                return fail (ErrorCode::notSeekable, r,
                             "seeking to the start failed and the source cannot be re-opened");
            }

            return reopen();
        }

        afterSeek (getStreamInfo().startPts, true);
        return {};
    }

    /// Re-establishes the source from scratch (non-seekable sources that must rewind, or after a
    /// failed seek) and rebuilds the decoder on top of it.
    ///
    /// MediaSource::reopen() re-opens the container and returns: the container and the decoder have
    /// different lifetimes (this one container outlives two or three decoder rebuilds on the hardware
    /// fallback path), so the order of the two is decided here rather than buried in the re-open.
    /// buildCodec() is what resets the decode position, exactly as it does for every other rebuild.
    ///
    /// No re-open under an already-cancelled request: the re-probe runs its I/O through the interrupt
    /// callback and would keep only what it managed to read. Checked here rather than inside
    /// MediaSource, because by the time the container is touched this has already given up its
    /// decoder, and a cancelled request must not pay that.
    [[nodiscard]] std::expected<void, Error> reopen()
    {
        if (source.isCancelled())
        {
            return fail (ErrorCode::cancelled, k::exitRequested, "cancelled before re-opening the source");
        }

        const std::optional<HwCandidate> hw = decoder.getHardwareCandidate();
        decoder.releaseContext();
        position.markInvalid();
        auto r = source.reopen (opt);

        if (r) r = attachToSource();
        if (r) r = buildCodec (hw ? &*hw : nullptr);
        if (! r)
        {
            // The old context is gone and the new one failed; imageAt() retries on the next request.
            // close() is idempotent: MediaSource::reopen() has already closed when it was the step that
            // failed, and has not when attachToSource() or buildCodec() was.
            decoder.releaseContext();
            source.close();
            brokenReason = r.error().message;
            return fail (ErrorCode::seekFailed, r.error().avError, "re-opening the source: " + r.error().message);
        }

        tailEnd = k::noPts; // a re-opened (or grown) source may reach further than it did
        packets.resetVerifiedTo();
        position.markSeeked (getStreamInfo().startPts, /*atStart=*/true);
        return {};
    }

    /// Throws away everything the decoder and this pipeline believed after an interrupt aborted a
    /// libavformat read. MediaSource clears libavio's sticky state and flushes the demuxer; the
    /// decoder flush, the frontier and the position are this pipeline's to drop.
    ///
    /// Returns false when the sticky state could not be cleared in place (see
    /// MediaSource::recoverAfterInterrupt) and the source can be re-opened. The caller must then
    /// re-establish it before the next read; nothing else clears the I/O layer.
    [[nodiscard]] bool recoverAfterInterrupt() noexcept
    {
        const MediaSource::IoState io = source.recoverAfterInterrupt();

        if (io == MediaSource::IoState::clean) return true;
        const std::int64_t last = frontier.lastReceivedTs;
        decoder.flush();
        resetPosition();

        if (io == MediaSource::IoState::stuck && getStreamInfo().ioSeekable)
        {
            position.markInvalid(); // nothing here can unstick the I/O layer; the caller re-opens
            return false;
        }

        if (getStreamInfo().seekable)
        {
            position.markInvalid(); // forces a seek in positionFor()
        }
        else
        {
            // Best effort on a non-rewindable input: the demuxer is somewhere at or after the last frame.
            // The one field carried across reset(): the rest of the frontier described a decoder state
            // the flush above destroyed, but this one is still true of the source.
            position.markCarriedForward();
            frontier.lastReceivedTs = last;
        }

        return true;
    }

    /// Re-positions for `P` after an end of stream that produced no frames at all. libavio's sticky
    /// end-of-file is cleared first: a seek leaves it set, so the retry would read EOF again. On a
    /// libavformat major where it cannot be cleared in place, the source is re-opened instead.
    [[nodiscard]] std::expected<void, Error> retryAfterEmptyEof (std::int64_t P, const CancelToken& token)
    {
        if (! source.tryResetIo() && getStreamInfo().ioSeekable)
        {
            // An unverified libavformat major: the sticky end-of-file survives a seek, so the source is
            // re-established rather than retried in place. Slower by one re-open, and correct.
            resetPosition();

            if (auto r = reopen(); ! r) return r;
            return positionFor (P, keyframeOnly, token);
        }

        source.flushDemuxer();
        decoder.flush();
        resetPosition();
        position.markInvalid(); // forces a real seek rather than a decode-forward
        return positionFor (P, keyframeOnly, token);
    }

    /// Seeks towards `target`, with the failure policy: interrupted I/O is reported as is, other
    /// failures count towards giving up on seeking and fall back to a re-open.
    [[nodiscard]] std::expected<void, Error> seekOrReopen (std::int64_t target)
    {
        if (target <= getStreamInfo().startPts) return seekToStart();
        auto r = seekTo (target);

        if (r)
        {
            source.noteSeekSucceeded();
            return {};
        }

        if (r.error().avError == k::exitRequested) return r;
        source.noteSeekFailed();

        if (! getStreamInfo().ioSeekable)
        {
            return fail (ErrorCode::notSeekable, r.error().avError,
                         "seeking failed and the source cannot be re-opened");
        }

        return reopen();
    }

    [[nodiscard]] std::int64_t framesTicks (int n) const noexcept
    {
        return Positioner::framesTicks (getStreamInfo(), n);
    }

    /// What a positioning decision reads, gathered fresh at every call: a re-open replaces the
    /// AVStream the container view names, so a view must never outlive the decision it is handed to.
    [[nodiscard]] PositioningView positioningView() const noexcept
    {
        return PositioningView{ getStreamInfo(), keys, frontier, costs, position, containerIndexOf (source) };
    }

    /// Decides between continuing to decode forward and seeking, then positions accordingly. In
    /// keyframe mode the answer is a keyframe packet that is then decoded alone
    /// (armKeyframeDecode).
    [[nodiscard]] std::expected<void, Error> positionFor (std::int64_t P, bool keyframeMode, const CancelToken& token)
    {
        positioner.resetBackoff();

        if (! getStreamInfo().seekable)
        {
            if (Positioner::canDecodeForwardTo (P, positioningView())) return {};
            if (Positioner::isUndecodedStartLanding (positioningView())) return {};
            if (! getStreamInfo().ioSeekable)
            {
                return fail (ErrorCode::notSeekable,
                             "the source is not seekable; requested times must not precede the current position");
            }

            return reopen();
        }

        if (! keyframeMode)
        {
            if (Positioner::canDecodeForwardTo (P, positioningView())
                && positioner.isForwardCheaperThanSeek (P, positioningView()))
                return {};

            if (positioner.isUndecodedLandingBefore (P, positioningView()))
            {
                return {}; // freshly positioned just before the target; the landing is verified in
                           // select()
            }
        }

        if (getStreamInfo().indexTrusted) return positionIndexed (P, keyframeMode, token);
        return positionScanned (P, keyframeMode, token);
    }

    /// Containers with a trusted index (MP4, Matroska): seek to the target, read the landing
    /// keyframe packet, and correct an overshoot (fragmented MP4, open-GOP leading pictures) with a
    /// re-seek to the previous keyframe. Where each of those aims is Positioner's; this performs
    /// them and reads the landings in between.
    [[nodiscard]] std::expected<void, Error> positionIndexed (std::int64_t P, bool keyframeMode,
                                                              const CancelToken& token)
    {
        std::int64_t target = positioner.getIndexedAim (P);
        bool needSeek = true;

        for (int round = 0;; ++round)
        {
            if (needSeek)
            {
                if (auto r = seekOrReopen (target); ! r) return r;
            }

            needSeek = true;

            if (! getStreamInfo().seekable) return {}; // gave up on seeking: the re-open positioned at the start
            auto land = readLanding (P, /*scan=*/false, keyframeMode, token);

            if (! land) return std::unexpected (std::move (land.error()));
            if (land->found || position.isLandedAtStart() || ! packets.areKeyFlagsReliable()) return {};
            if (land->firstKeyPts == k::noPts)
            {
                // No keyframe packet before the end: the seek landed in the tail. backOff() seeks itself.
                if (auto s = backOff (P, k::noPts); ! s) return s;
                needSeek = false;
                continue;
            }

            // Overshoot: the landing keyframe is past P. One cheap seek to the keyframe strictly before
            // it, or the start once the rounds are spent.
            const Positioner::Aim aim =
                positioner.retryAfterOvershoot (P, land->firstKeyPts, land->firstKeyDts, round, positioningView());

            if (aim.toStart) return seekToStartAndLand (P, keyframeMode, token);
            target = aim.target;
        }
    }

    [[nodiscard]] std::expected<void, Error> seekToStartAndLand (std::int64_t P, bool keyframeMode,
                                                                 const CancelToken& token)
    {
        if (auto r = seekToStart(); ! r) return r;
        auto land = readLanding (P, /*scan=*/! getStreamInfo().indexTrusted, keyframeMode, token);

        if (! land) return std::unexpected (std::move (land.error()));
        return {};
    }

    /// Containers without a trusted index (MPEG-TS): position with a byte seek when the keyframe
    /// covering P is already known; otherwise aim one GOP early, scan the packets up to the target
    /// and choose the last keyframe at or before it (recording every keyframe on the way).
    [[nodiscard]] std::expected<void, Error> positionScanned (std::int64_t P, bool keyframeMode,
                                                              const CancelToken& token)
    {
        if (const KeyEntry* e = keys.findCoveringKey (P); e != nullptr)
        {
            if (auto r = byteSeek (*e); ! r) return r;
            if (! keyframeMode) return {};
            auto land = readLanding (P, /*scan=*/false, keyframeMode, token);

            if (! land) return std::unexpected (std::move (land.error()));
            if (land->found) return {};
            // The recorded position no longer holds (the file changed): fall through to a fresh scan.
        }

        if (auto r = seekOrReopen (Positioner::getScanAim (P, positioningView())); ! r) return r;
        for (;;)
        {
            if (! getStreamInfo().seekable) return {};
            auto land = readLanding (P, /*scan=*/true, keyframeMode, token);

            if (! land) return std::unexpected (std::move (land.error()));
            if (land->found || position.isLandedAtStart() || ! packets.areKeyFlagsReliable()) return {};
            // Landed after the keyframe covering P (or in the tail): retreat by a growing step.
            if (auto s = backOff (P, land->firstKeyPts); ! s) return s;
        }
    }

    /// Retreats after a landing proved to be past `P`: Positioner picks the new target (or gives
    /// up and sends us to the start), this performs the seek it asked for.
    [[nodiscard]] std::expected<void, Error> backOff (std::int64_t P, std::int64_t observedKey /* or k::noPts */)
    {
        const Positioner::Aim aim = positioner.nextBackoff (P, observedKey, positioningView());

        if (aim.toStart) return seekToStart();
        return seekOrReopen (aim.target);
    }

    [[nodiscard]] std::int64_t getFrameTs (const AVFrame& f) const noexcept
    {
        if (f.best_effort_timestamp != k::noPts) return f.best_effort_timestamp;
        if (f.pts != k::noPts) return f.pts;
        if (f.pkt_dts != k::noPts) return f.pkt_dts;
        return frontier.synthTs != k::noPts ? frontier.synthTs : getStreamInfo().startPts;
    }

    [[nodiscard]] std::int64_t getFrameDuration (const AVFrame& f) const noexcept
    {
        return f.duration > 0 ? f.duration : getStreamInfo().frameDurationHint;
    }

    // The received frame becomes the answer. The look-ahead slot is emptied with it: no frame past
    // the new held one has been seen yet, and a stale one there would claim it covers the request.
    void holdReceived (bool concealed) noexcept
    {
        held.adopt (decoder.getFrame(), concealed);
        pending.clear();
    }

    // One frame of look-ahead past the held frame -- what proves the held frame covers the request.
    void pendReceived (bool concealed) noexcept { pending.adopt (decoder.getFrame(), concealed); }

    /// Pulls one frame into the decoder's frame. Returns 0, k::eof, k::eagain (needs a packet) or
    /// an error.
    ///
    /// The one thing added to VideoDecoder::receive(): once the drain has started, "nothing yet" is
    /// the end of the stream rather than a request for another packet. Whether a drain is in
    /// progress is the decode loop's own state, so the translation is made here and not inside the
    /// decoder.
    [[nodiscard]] int receiveOne() noexcept
    {
        const int r = decoder.receive();

        if (r == k::eagain && frontier.draining) return k::eof;
        return r;
    }

    /// Sends the reader's live packet. Returns 0 (consumed), k::eagain (kept for a retry after the
    /// decoder has been drained) or an error (packet dropped).
    [[nodiscard]] int sendHeldPacket() noexcept
    {
        AVPacket& pkt = packets.getPacket();
        const std::int64_t fedDts = pkt.dts;
        const std::int64_t fedPts = pkt.pts;
        const int s = decoder.send (pkt);

        if (s == k::eagain)
        {
            packets.holdPacketForRetry(); // the decoder wants a receive first; keep the packet
            return s;
        }

        if (s == 0)
        {
            if (fedDts != k::noPts) frontier.lastFedDts = fedDts;
            if (fedPts != k::noPts)
                frontier.lastFedPts = frontier.lastFedPts == k::noPts ? fedPts : std::max (frontier.lastFedPts, fedPts);
        }

        packets.releasePacket();
        return s;
    }

    /// PacketReader::readLanding() plus the part only positioning may carry out. The scan reads
    /// packets and chooses the keyframe; going back to one is a seek, and seeks are not the
    /// reader's. Returned as data rather than done in place so the dependency runs one way:
    /// positioning drives the reader, never the other way round.
    [[nodiscard]] std::expected<PacketReader::Landing, Error> readLanding (std::int64_t P, bool scan, bool keyframeMode,
                                                                           const CancelToken& token)
    {
        auto land = packets.readLanding (source, keys, P, scan, keyframeMode, frontier.tainted, token);

        if (! land) return land;
        if (land->awaitKey) position.markAwaitingKey();
        switch (land->rewind)
        {
        case PacketReader::Landing::Rewind::none:
            break;
        case PacketReader::Landing::Rewind::byteSeek:
            if (auto r = byteSeek (land->rewindEntry); ! r) return std::unexpected (std::move (r.error()));
            break;
        case PacketReader::Landing::Rewind::timestampSeek:
            if (auto r = seekOrReopen (land->rewindTs); ! r) return std::unexpected (std::move (r.error()));
            break;
        }

        return land;
    }

    /// Keyframe mode after positioning: feed the pending keyframe packet and drain, so the decoder
    /// emits that one frame at once instead of after a pipeline's worth of packets (frame threads
    /// hold ~thread_count packets). The decoder must be flushed before it is fed again
    /// (`frontier.drained`).
    [[nodiscard]] std::expected<void, Error> armKeyframeDecode (const CancelToken& token)
    {
        if (! packets.isPacketPending())
        {
            // Positioned but not read yet: fetch the keyframe packet.
            auto land = readLanding (std::numeric_limits<std::int64_t>::max(), /*scan=*/false,
                                     /*keyframeMode=*/true, token);

            if (! land) return std::unexpected (std::move (land.error()));
            if (! packets.isPacketPending()) return {}; // nothing to feed (EOF); select() reports it
        }

        if (! packets.areKeyFlagsReliable() || packets.getPacket().pts == k::noPts)
            return {}; // cannot single out a keyframe: decode normally
        position.noteKeyframeFed();
        const int s = sendHeldPacket();

        if (s < 0 && s != k::eagain) return fail (ErrorCode::decodeFailed, s, "avcodec_send_packet (keyframe)");
        decoder.startDrain();
        frontier.draining = true;
        frontier.drained = true;
        position.markLandingKnown();
        return {};
    }

    /// Reads the next packet of our stream and feeds it. Returns 0, k::eof (drain started),
    /// k::exitRequested (interrupted) or a decoder error.
    /// Not noexcept: the skipped-frame record below it allocates.
    [[nodiscard]] int feedOne (const CancelToken& token)
    {
        if (packets.isPacketPending())
        {
            if ((packets.getPacket().flags & AV_PKT_FLAG_KEY) != 0) position.noteKeyframeFed();
            prepareSend();
            const int s = sendHeldPacket();

            if (s == k::eagain)
            {
                // Both receive and send report EAGAIN: the decoder violates its contract.
                packets.releasePacket();
                return k::einval;
            }

            if (s == 0) return 0;
            if (s != k::invalidData) return s;
            ++totalDecodeErrors;
            frontier.tainted = true;
        }

        if (frontier.drained) return k::eof; // the decoder was drained for a keyframe-only decode; a seek resets it
        for (;;)
        {
            const int r = packets.readVideoPacket (source, keys, frontier.tainted, token);

            if (r == k::exitRequested) return r;
            if (r == k::eof)
            {
                frontier.draining = true;
                decoder.startDrain();
                return k::eof;
            }

            const bool keyPacket = (packets.getPacket().flags & AV_PKT_FLAG_KEY) != 0;

            if (position.isAwaitingKey() && packets.areKeyFlagsReliable() && ! keyPacket)
            {
                // Mid-GOP after a seek: the decoder would decode these in full and drop them anyway.
                packets.unrefPacket();
                continue;
            }

            if (keyPacket) position.noteKeyframeFed();
            prepareSend();
            const int s = sendHeldPacket();

            if (s == 0 || s == k::eagain) return 0;
            if (s == k::invalidData)
            {
                ++totalDecodeErrors;
                frontier.tainted = true;

                if (++decodeErrors > maxConsecutiveErrors) return s;
                continue;
            }

            return s;
        }
    }

    /// Before a packet goes to the decoder: non-reference frames far before the target are skipped
    /// at the decoder level (AVDISCARD_NONREF).
    ///
    /// A frame may be skipped only when its display interval provably ends before the window.
    /// Durations cannot prove that (mov stores decode-order deltas, so a VFR stream's frame may say
    /// "1/30 s"), nor can the average frame rate. What does: a frame with a *later* presentation
    /// time has already been fed (B-frames follow their forward reference in decode order) and that
    /// later time is itself before the window.
    void prepareSend()
    {
        const AVPacket& pkt = packets.getPacket();
        const bool key = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
        AVDiscard skip = AVDISCARD_DEFAULT;

        if (skipBeforeTs != k::noPts && packets.areKeyFlagsReliable() && ! key && pkt.pts != k::noPts
            && frontier.lastFedPts != k::noPts)
        {
            const std::int64_t pts = pkt.pts;

            if (pts < frontier.lastFedPts && frontier.lastFedPts <= skipBeforeTs)
            {
                skip = AVDISCARD_NONREF;
                frontier.skipped.record (pts); // if the decoder drops it, nothing may decode across it
            }
        }

        decoder.setSkipPolicy (skip);
    }

    /// Core selection loop. Precondition: positioned (or continuing forward).
    [[nodiscard]] std::expected<Selected, Error> select (std::int64_t P, std::int64_t lo, std::int64_t hi,
                                                         const CancelToken& token)
    {
        for (;;)
        {
            if (token.isRequested()) return fail (ErrorCode::cancelled, "cancelled");
            const int r = receiveOne();

            if (r == 0)
            {
                decodeErrors = 0;
                costs.noteFirstFrame();

                if (getStreamInfo().synthesizeTimestamps)
                {
                    const std::int64_t stamped =
                        frontier.synthTs != k::noPts ? frontier.synthTs : getStreamInfo().startPts;
                    AVFrame& got = decoder.getFrame();
                    got.pts = got.best_effort_timestamp = stamped;
                    got.duration = getStreamInfo().frameDurationHint;
                }

                const AVFrame& got = decoder.getFrame();
                const std::int64_t t = getFrameTs (got);
                frontier.synthTs = t + getFrameDuration (got);
                frontier.lastReceivedTs = t;
                ++frontier.receivedSinceSeek;
                const bool key = (got.flags & AV_FRAME_FLAG_KEY) != 0;
                // GOP length estimate (a lower bound, exact at the next keyframe): feeds the back-off step
                // and the forward-scan limit.
                keys.noteKeyframeSpan (frontier.lastKeyTs, t);

                if (key) frontier.lastKeyTs = t;
                if ((got.flags & AV_FRAME_FLAG_CORRUPT) != 0)
                {
                    frontier.skipped.record (t); // not produced: nothing may decode across it
                    // Concealed by construction: this is the frame the decoder itself flagged, stashed only
                    // as a fallback for a stream that never produces a better one (finishAtEof).
                    corruptLast.adopt (decoder.getFrame(), /*wasConcealed=*/true);
                    continue;
                }

                // Corrupt: the decoder reported concealment, or a decode error occurred since the last
                // keyframe (the reference chain is suspect).
                if (key) frontier.tainted = false;
                const bool concealed = got.decode_error_flags != 0 || frontier.tainted;
                frontier.lastEnd = std::max (frontier.lastEnd, t + getFrameDuration (got));
                frontier.skipped.clearAt (t); // whatever was skipped here has now been produced

                if (keyframeOnly)
                {
                    // "The keyframe at or before P": the fed keyframe after a packet-level landing
                    // (isLandingKnown), or the first keyframe out after a seek aimed at P itself. After a
                    // back-off (the seek target is below P) that guarantee is gone: keep decoding,
                    // remembering the last keyframe <= P, until a frame past P shows up.
                    if (t <= P)
                    {
                        if (key || ! held.isValid()) holdReceived (concealed); // a non-key frame only as a fallback
                        if (key
                            && (position.isLandingKnown() || t == P
                                || (frontier.receivedSinceSeek == 1 && position.getSeekTarget() == P)))
                        {
                            return Selected{ held.getFrame(), Adjustment::none, held.isConcealed() };
                        }

                        continue;
                    }

                    if (held.isValid())
                    {
                        pendReceived (concealed);
                        return Selected{ held.getFrame(), Adjustment::none, held.isConcealed() };
                    }
                }
                else
                {
                    if (t < P)
                    {
                        holdReceived (concealed);

                        if (t >= lo)
                            return Selected{ held.getFrame(), Adjustment::none,
                                             held.isConcealed() }; // early accept within tolerance
                        continue;
                    }

                    if (t == P)
                    {
                        holdReceived (concealed);
                        return Selected{ held.getFrame(), Adjustment::none, held.isConcealed() };
                    }

                    if (held.isValid())
                    {
                        pendReceived (concealed);
                        return Selected{ held.getFrame(), Adjustment::none, held.isConcealed() };
                    }
                }

                // Overshoot: the first frame out is already past P. Either P precedes the first frame of
                // the stream (provable only after the explicit start seek) or the seek landed late. A late
                // landing is backed off even when a later frame would satisfy the `after` tolerance: the
                // frame actually on screen at P exists until proven otherwise.
                const bool atStart = position.isLandedAtStart() || ! getStreamInfo().seekable
                                     || (firstFrameTs != k::noPts && t <= firstFrameTs);
                const bool landing = frontier.receivedSinceSeek == 1;
                const std::int64_t observedKey = key ? t : k::noPts;

                if (landing && ! atStart)
                {
                    if (auto s = backOff (P, observedKey); ! s) return std::unexpected (std::move (s.error()));
                    continue;
                }

                if (t <= hi)
                {
                    holdReceived (concealed);
                    return Selected{ held.getFrame(), Adjustment::none, held.isConcealed() };
                }

                if (atStart)
                {
                    holdReceived (concealed);
                    return Selected{ held.getFrame(), Adjustment::clampedToFirst, held.isConcealed() };
                }

                if (auto s = backOff (P, observedKey); ! s) return std::unexpected (std::move (s.error()));
                continue;
            }

            if (r == k::eof)
            {
                // Nothing decodable between the landing and the end of the stream. Back off unless the
                // start seek has already been done, in which case the stream really has no frame for P.
                if (! held.isValid() && ! corruptLast.isValid() && getStreamInfo().seekable && position.isPositioned()
                    && ! position.isLandedAtStart() && ! frontier.drained)
                {
                    if (auto s = backOff (P, k::noPts); ! s) return std::unexpected (std::move (s.error()));
                    continue;
                }

                // Not one frame came out of the decoder since this request positioned. On a seekable
                // source that says nothing about the stream: the start seek landed and the very first read
                // reported the end, which is what libavio's sticky eof_reached does after a cancellation
                // (a seek does not clear it). Clear it and re-position once before concluding the stream
                // ended — the landed-at-start test above would otherwise accept that first read as proof.
                if (! held.isValid() && ! corruptLast.isValid() && getStreamInfo().seekable && ! frontier.drained
                    && frontier.receivedSinceSeek == 0 && ! eofRetried)
                {
                    eofRetried = true;

                    if (auto s = retryAfterEmptyEof (P, token); ! s) return std::unexpected (std::move (s.error()));
                    continue;
                }

                frontier.eof = true;

                if (frontier.lastEnd != std::numeric_limits<std::int64_t>::min())
                {
                    tailEnd = tailEnd == k::noPts ? frontier.lastEnd : std::max (tailEnd, frontier.lastEnd);
                }

                return finishAtEof (P);
            }

            if (r == k::eagain)
            {
                const int f = feedOne (token);

                if (f == 0 || f == k::eof) continue;
                if (f == k::exitRequested)
                {
                    if (token.isRequested()) return fail (ErrorCode::cancelled, "cancelled");
                    return fail (ErrorCode::decodeFailed, f, "av_read_frame: interrupted without a cancellation");
                }

                if (decoder.isHardwareFault (f))
                {
                    decoder.noteHardwareFault();
                    return fail (ErrorCode::decodeFailed, f, "avcodec_send_packet (hardware)");
                }

                return fail (ErrorCode::decodeFailed, f, "avcodec_send_packet");
            }

            ++totalDecodeErrors;
            frontier.tainted = true;

            if (decoder.isHardwareFault (r))
            {
                decoder.noteHardwareFault();
                return fail (ErrorCode::decodeFailed, r, "avcodec_receive_frame (hardware)");
            }

            if (++decodeErrors > maxConsecutiveErrors)
            {
                return fail (ErrorCode::decodeFailed, r, "avcodec_receive_frame: too many consecutive errors");
            }
        }
    }

    [[nodiscard]] std::expected<Selected, Error> finishAtEof (std::int64_t P)
    {
        const auto finish = [&] (AVFrame* f, bool corrupt) -> std::expected<Selected, Error>
        {
            const std::int64_t t = getFrameTs (*f);
            // In keyframe mode the stream extends past the held keyframe to the last frame decoded after
            // it.
            const std::int64_t end = std::max (t + getFrameDuration (*f), keyframeOnly ? frontier.lastEnd : t);

            if (P < end) return Selected{ f, Adjustment::none, corrupt };
            if (opt.outOfRange == OutOfRangePolicy::clampToLastFrame)
                return Selected{ f, Adjustment::clampedToLast, corrupt };
            return fail (ErrorCode::timeOutOfRange,
                         "requested time is past the last frame ("
                             + toString (Time::fromTimestamp (std::max<std::int64_t> (t - getStreamInfo().startPts, 0),
                                                              fromAv (getStreamInfo().timeBase)))
                             + ")");
        };

        if (held.isValid()) return finish (held.getFrame(), held.isConcealed());
        if (corruptLast.isValid())
        {
            held.adopt (corruptLast); // carries the concealed flag the stash was adopted with

            // Nearest-keyframe mode asks for "the keyframe at or before P", and a stashed frame at or
            // before P answers that: its display interval is not the question, as it is in exact mode.
            if (keyframeOnly && P >= getFrameTs (*held.getFrame()))
                return Selected{ held.getFrame(), Adjustment::none, true };
            return finish (held.getFrame(), true);
        }

        if (totalDecodeErrors > 0)
        {
            return fail (ErrorCode::decodeFailed,
                         "no decodable frame found (" + std::to_string (totalDecodeErrors) + " decode errors)");
        }

        return fail (ErrorCode::endOfStream, "the stream ended before a frame for the requested time was decoded");
    }

    /// Nearest-keyframe mode answers with the keyframe at or before P without decoding past it, so it
    /// cannot tell a long GOP from the tail of a truncated file: a request past the end would come
    /// back as the last keyframe, unflagged, where exact mode reports timeOutOfRange. When the
    /// chosen keyframe is more than a GOP behind the request, read ahead (packets only) until one
    /// proves the stream reaches P or the source ends. Remembered in tailEnd: once per source.
    [[nodiscard]] std::expected<Selected, Error> verifyKeyframeTail (Selected sel, std::int64_t P,
                                                                     const CancelToken& token)
    {
        if (sel.frame == nullptr) return sel;
        const std::int64_t t = getFrameTs (*sel.frame);
        const std::int64_t gop = std::max ({ keys.getGopHint(), framesTicks (2), std::int64_t{ 1 } });

        if (P <= t + gop || P <= packets.getVerifiedTo()) return sel; // plainly inside the data
        if (tailEnd == k::noPts)
        {
            // The demuxer sits just after the chosen keyframe: read on until the question is answered.
            for (;;)
            {
                const int r = packets.readVideoPacket (source, keys, frontier.tainted, token);

                if (r == k::exitRequested)
                {
                    if (token.isRequested()) return fail (ErrorCode::cancelled, "cancelled");
                    return sel; // an I/O hiccup is not proof of anything: keep the keyframe
                }

                if (r == k::eof)
                {
                    const std::int64_t verified = packets.getVerifiedTo();
                    tailEnd = verified != k::noPts
                                  ? verified + std::max<std::int64_t> (getStreamInfo().frameDurationHint, 1)
                                  : t + std::max<std::int64_t> (getStreamInfo().frameDurationHint, 1);
                    break;
                }

                packets.unrefPacket();
                const std::int64_t verified = packets.getVerifiedTo();

                if (verified != k::noPts && verified > P) break;
            }

            position.markInvalid(); // the demuxer has moved; the next request repositions
        }

        if (tailEnd == k::noPts || P < tailEnd) return sel;
        if (opt.outOfRange == OutOfRangePolicy::clampToLastFrame)
        {
            sel.adjustment = Adjustment::clampedToLast;
            return sel;
        }

        return fail (ErrorCode::timeOutOfRange, "requested time is past the last frame ("
                                                    + toString (Time::fromTimestamp (
                                                        std::max<std::int64_t> (tailEnd - getStreamInfo().startPts, 0),
                                                        fromAv (getStreamInfo().timeBase)))
                                                    + ")");
    }

    /// Whether the held frame really is the last frame in presentation order of everything decoded
    /// since positioning. It is, for every monotonic stream. It is not on a container whose
    /// timestamps jump backwards (two recordings concatenated into one MPEG-TS, a camera restart):
    /// decoding forward across the jump ends with a frame from the *earlier* segment, and concluding
    /// "the stream has no frame past this one" from it strands the generator, because the
    /// end-of-stream shortcut then answers every later request without ever repositioning.
    [[nodiscard]] bool isHeldFrameAtTail() const noexcept
    {
        if (! held.isValid()) return false;
        return getFrameTs (*held.getFrame()) + getFrameDuration (*held.getFrame()) >= frontier.lastEnd;
    }

    /// Whether the held frame is the one on screen at `target`, as far as the frontier can say.
    ///
    /// The invariant: the held frame's display interval starts at or before `target` and every frame
    /// between the two was produced. A frame skipped in between is the one on screen instead, so the
    /// held frame does not cover `target` after all, whatever the look-ahead or the end of stream
    /// say. What rules out a *later* frame covering `target` is mode-specific and stays with the
    /// caller: the look-ahead frame or the end of stream in exact mode, the absence of a keyframe in
    /// (held, target] in nearest-keyframe mode.
    [[nodiscard]] bool isHeldFrameCovering (std::int64_t target) const noexcept
    {
        if (! held.isValid()) return false; // guards getFrame()
        const std::int64_t h = getFrameTs (*held.getFrame());
        return h <= target && ! frontier.skipped.containsIn (h, target);
    }

    /// Whether the look-ahead frame can become the held frame and the decode continue from there.
    ///
    /// The invariant: the pending frame begins at or before `target`, so the frame on screen at
    /// `target` is it or one the decoder has yet to produce; every frame between it and `target` will
    /// be produced; and reaching `target` that way costs less than seeking to it.
    [[nodiscard]] bool canPromotePendingFrame (std::int64_t target) const noexcept
    {
        if (! pending.isValid() || frontier.drained) return false; // isValid() guards getFrame()
        const std::int64_t p = getFrameTs (*pending.getFrame());
        return target >= p && ! frontier.skipped.containsIn (p, target)
               && positioner.isForwardCheaperThanSeek (target, positioningView());
    }

    /// Whether the request can be answered without repositioning, by decoding on from the held frame.
    ///
    /// The invariant: the decoder's next output is the frame after the held one, and every frame
    /// between the held frame and `target` will be produced. An empty look-ahead slot is what says
    /// nothing past the held frame has been received; `eof` and `drained` say the decoder has no more
    /// to give without a flush; a recorded hole says one of the frames in between was skipped and
    /// never will be produced. Any of those failing leaves the frame on screen at `target`
    /// unestablishable from here, and the request has to position.
    [[nodiscard]] bool canContinueFromHeldFrame (std::int64_t target) const noexcept
    {
        if (! held.isValid() || pending.isValid() || frontier.eof || frontier.drained)
        {
            return false; // isValid() guards getFrame()
        }

        const std::int64_t h = getFrameTs (*held.getFrame());
        return h <= target && ! frontier.skipped.containsIn (h, target)
               && positioner.isForwardCheaperThanSeek (target, positioningView());
    }

    /// Positions and selects (the caller converts).
    [[nodiscard]] std::expected<Selected, Error> extract (std::int64_t P, std::int64_t lo, std::int64_t hi,
                                                          Adjustment clampedByBounds, const CancelToken& token)
    {
        const bool infiniteBefore = lo == std::numeric_limits<std::int64_t>::min();
        // Nearest-keyframe mode (infinite `before`). Without a usable seek every frame is decoded
        // anyway, so fall back to exact selection instead of returning an arbitrary non-keyframe.
        const bool keyframeMode = infiniteBefore && getStreamInfo().seekable;

        if (infiniteBefore && ! getStreamInfo().seekable)
        {
            lo = P;
            hi = P;
        }

        // Fast path: the frame covering P is already held, and nothing later covers it instead.
        if (isHeldFrameCovering (P))
        {
            if (keyframeMode)
            {
                // The held frame is the keyframe covering P (the index says no keyframe lies in (h, P]).
                const std::int64_t h = getFrameTs (*held.getFrame());

                if ((held.getFrame()->flags & AV_FRAME_FLAG_KEY) != 0
                    && keys.doesKeyCover (h, P, containerIndexOf (source)))
                {
                    return verifyKeyframeTail (Selected{ held.getFrame(), clampedByBounds, held.isConcealed() }, P,
                                               token);
                }
            }
            else
            {
                if (pending.isValid() && P < getFrameTs (*pending.getFrame()))
                {
                    return Selected{ held.getFrame(), clampedByBounds, held.isConcealed() };
                }

                if (frontier.eof && ! pending.isValid() && isHeldFrameAtTail())
                {
                    auto s = finishAtEof (P);

                    if (! s) return std::unexpected (std::move (s.error()));
                    if (s->adjustment == Adjustment::none) s->adjustment = clampedByBounds;
                    return *s;
                }
            }
        }

        keyframeOnly = keyframeMode;
        // Frames well before the tolerance window may skip their non-reference members. Off in
        // keyframe mode and on inputs without keyframe flags.
        skipBeforeTs = k::noPts;

        if (! keyframeMode && packets.areKeyFlagsReliable())
        {
            const std::int64_t margin = keys.getReorderTicks() + framesTicks (2);
            skipBeforeTs = lo > std::numeric_limits<std::int64_t>::min() + margin ? lo - margin : lo;
        }

        if (keyframeMode)
        {
            if (auto r = positionFor (P, true, token); ! r) return std::unexpected (std::move (r.error()));
            const AVPacket& pkt = packets.getPacket();
            const bool preEdit = packets.isPacketPending()
                                 && ((pkt.flags & AV_PKT_FLAG_DISCARD) != 0
                                     || (firstFrameTs != k::noPts && pkt.pts != k::noPts && pkt.pts < firstFrameTs));

            if (preEdit)
            {
                // The keyframe covering P precedes the first presented frame (an edit list trimmed its
                // GOP). Answer with the first presented frame, clamped, decoded exactly: the decoder drops
                // the trimmed frames itself (AV_PKT_FLAG_DISCARD).
                keyframeOnly = false;
                P = lo = hi = firstFrameTs != k::noPts ? firstFrameTs : getStreamInfo().startPts;
                clampedByBounds = Adjustment::keyframeBeforeEdit;
            }
            else if (auto r = armKeyframeDecode (token); ! r)
            {
                return std::unexpected (std::move (r.error()));
            }
        }
        else if (canPromotePendingFrame (P))
        {
            held.adopt (pending);
            positioner.resetBackoff();
        }
        else if (! canContinueFromHeldFrame (P))
        {
            if (auto r = positionFor (P, false, token); ! r) return std::unexpected (std::move (r.error()));
        }
        else
        {
            positioner.resetBackoff();
        }

        costs.beginRequest();
        const int framesBefore = getDecodedFrameCount();
        const bool seeked = getSeekCount() > 0;
        auto sel = select (P, lo, hi, token);

        if (! sel)
        {
            // A request abandoned before reaching its target fed packets under *its* skip window, and
            // only it knew where that window was: the frames between the decoder's frontier and the
            // window may never be produced, so nothing may continue forward across it. The next request
            // repositions — one seek per cancellation. A source that cannot be repositioned (a pipe)
            // relies on the recorded holes instead, and a request across one fails rather than lies.
            if (skipBeforeTs != k::noPts && getStreamInfo().seekable) position.markInvalid();
            return std::unexpected (std::move (sel.error()));
        }

        costs.learn (getDecodedFrameCount() - framesBefore, seeked);

        if (sel->adjustment == Adjustment::none) sel->adjustment = clampedByBounds;
        if (keyframeMode) return verifyKeyframeTail (*sel, P, token);
        return *sel;
    }

    [[nodiscard]] std::expected<Image, Error> convert (Selected sel)
    {
        const AVFrame* src = sel.frame;
        FramePtr sw;

        if (src->hw_frames_ctx != nullptr)
        {
            auto t = converter->download (*src);

            if (! t)
            {
                decoder.noteHardwareFault();
                return std::unexpected (std::move (t.error()));
            }

            sw = std::move (*t);
            src = sw.get();
        }

        auto out = converter->convert (*src, getStreamInfo().containerSar, getStreamInfo().codecSar, reqMax);

        if (! out) return std::unexpected (std::move (out.error()));
        // A frame before the time origin (MPEG-TS whose start comes from another stream) is reported at
        // zero.
        const std::int64_t t = std::max<std::int64_t> (getFrameTs (*sel.frame) - getStreamInfo().startPts, 0);
        const Time actual = Time::fromTimestamp (t, fromAv (getStreamInfo().timeBase));
        const bool key = (sel.frame->flags & AV_FRAME_FLAG_KEY) != 0;
        const ColorRange range = fromAv (static_cast<AVColorRange> ((*out)->color_range));
        const std::int64_t dur = getFrameDuration (*sel.frame);
        (*out)->duration = dur;
        Image img = ImageAccess::make (std::move (*out), opt.pixelFormat, range,
                                       actual.isValid() ? actual : Time::zero(), key, sel.adjustment, sel.corrupt);

        if (dur > 0) ImageAccess::setDuration (img, Time::fromTimestamp (dur, fromAv (getStreamInfo().timeBase)));
        return img;
    }

    MediaSource source;
    Options opt;
    VideoDecoder decoder;

    AssetInfo info;
    std::optional<Converter> converter;

    KeyframeIndex keys; ///< where the keyframes are; the reader records into it as it reads
    PacketReader packets;
    Positioner positioner; ///< decides where to seek; the seeking itself is done here
    FrameSlot held, pending, corruptLast;
    DecodeFrontier frontier; ///< where the decoder is; every reposition resets it
    Position position;       ///< where the demuxer is; written only through its own verbs
    SeekCostModel costs;     ///< what a seek and a frame cost; drives isForwardCheaperThanSeek()
    const AVFrame* probeFrame{ nullptr };
    /// One re-position per request after an end of stream that decoded nothing (see select()).
    bool eofRetried{ false };
    bool keyframeOnly{ false }; ///< the current request is in nearest-keyframe mode
    std::int64_t firstFrameTs{ k::noPts };
    std::optional<Size> reqMax;            ///< RequestOptions::maximumSize of the current request
    std::int64_t skipBeforeTs{ k::noPts }; ///< packets whose frames end before this may skip non-reference frames
    std::int64_t tailEnd{ k::noPts };      ///< end of the data once the end of stream has been observed
                                           ///< (k::noPts = not yet)
    int decodeErrors{ 0 };
    long totalDecodeErrors{ 0 };
    /// Why this pipeline is currently unusable: a re-open that failed, or a software decoder that
    /// could not be rebuilt after a hardware failure. Not the source's and not the decoder's —
    /// either of them being dead is what makes the pipeline dead, and the one reader
    /// (selectFor's "the generator is unusable") tests both. The pipeline retries on the next
    /// request.
    std::string brokenReason;

    /// Where the sub-objects' lifetime counters stood when the current request started, reset at
    /// the start of each one. Not observability: SeekCostModel::learn() is fed the frames this
    /// request decoded, and isForwardCheaperThanSeek() decides seek-versus-decode-forward from the
    /// averages it maintains. Decoding thread only.
    struct RequestStats
    {
        /// MediaSource's lifetime seek count when this request started; getSeekCount() differences it.
        std::int64_t seeksAtStart{ 0 };
        /// VideoDecoder's lifetime frame count when this request started; getDecodedFrameCount()
        /// differences it. Counts look-ahead and skipped frames, like the counter it comes from.
        std::int64_t framesAtStart{ 0 };
    };

    RequestStats cur;
};

} // namespace stills::detail

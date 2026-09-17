#pragma once
// stills/detail/stills_VideoDecoder.h — the AVCodecContext, the hardware session behind it, and the
// one frame the decoder writes its output into.
//
// VideoDecoder owns the decoder and nothing else: it builds a codec context (software, or over a
// hardware device it creates and keeps), sends packets, receives frames, flushes, drains, and
// publishes a snapshot of which path it ended up on. It does not read packets, does not decide
// where to position, and does not choose which frame answers a request. It is asked to decode and
// it decodes.
//
// What it deliberately does not do. Probing — decoding the first frame of the stream to find out
// whether a candidate really works — runs the selection loop against the real source, so it stays
// in FramePipeline; what is the decoder's own is "which hardware candidate am I currently using"
// (getHardwareCandidate), which is what lets the caller rebuild the same one. For the same reason
// build() neither drops the frames decoded from the decoder it replaces nor tells the source what
// frame rate the decoder negotiated: both are the owner's, and the owner does them around the call.
//
// Threading. Unlike the other detail types this one is not simply "single-threaded". Everything
// here belongs to the thread running the pipeline's decode loop — the codec context, the hardware
// device and state, the received frame, the counters and the fault flags — with exactly one
// exception: the ActiveDecoder snapshot. getActiveDecoder() is called from other threads while
// that decode loop runs (a caller asking which decoder it ended up with, see stills_AssetImageGenerator.h), so
// the snapshot is written under activeMutex and read back under it, by value. Nothing else here
// may be touched concurrently and nothing else needs to be: the only thing another thread may do
// to a VideoDecoder is call getActiveDecoder().
//
// Why a snapshot and not a reference: a hardware decoder can fail lazily, long after open(), and
// the software fallback rewrites every field of the value. A reader holding a reference to it
// would observe the change field by field.

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_HardwareSupport.h"
#include "stills/stills_AssetInfo.h"
#include "stills/stills_Error.h"
#include "stills/stills_Options.h"

namespace stills::detail
{

class VideoDecoder
{
public:
    /// Remembers the decoder libavformat picked for the stream and allocates the frame every
    /// receive() lands in. Runs after every MediaSource open and re-open; the frame outlives any
    /// number of codec contexts built on top of it.
    [[nodiscard]] std::expected<void, Error> attach (const AVCodec* decoderForStream)
    {
        codecDesc = decoderForStream;
        auto f = makeFrame();

        if (! f) return std::unexpected (f.error());
        recv = std::move (*f);
        return {};
    }

    /// Builds a codec context for `par`, over the hardware candidate `hw` (nullptr = software), and
    /// opens it. Replaces whatever was built before, hardware session included.
    ///
    /// On failure the decoder is left unbuilt (isBuilt() is false) but a hardware *device* created
    /// on the way may survive, which is what lets a failed candidate still be named in the attempts
    /// string. The caller drops everything it decoded from the old context before calling this:
    /// nothing decoded by a decoder that no longer exists describes where the new one is.
    [[nodiscard]] std::expected<void, Error> build (const AVCodecParameters& par, AVRational timeBase,
                                                    const Options& opt, const HwCandidate* hw)
    {
        hwFault = false; // belongs to the decoder being replaced
        codec.reset();
        hwDevice.reset();
        hwState.reset();

        CodecCtxPtr cc{ avcodec_alloc_context3 (codecDesc) };

        if (! cc) return fail (ErrorCode::outOfMemory, "avcodec_alloc_context3");
        if (int r = avcodec_parameters_to_context (cc.get(), &par); r < 0)
        {
            return fail (ErrorCode::decoderOpenFailed, r, "avcodec_parameters_to_context");
        }

        // required for best_effort_timestamp / frame->duration
        cc->pkt_timebase = timeBase;

        // The same cap inside libavcodec, so a resolution change mid-stream is refused by the decoder
        // rather than allocated for.
        if (opt.maxInputPixels) cc->max_pixels = *opt.maxInputPixels;
        // Frame threading adds latency after every flush but still wins for this seek-heavy workload.
        // Hardware decoders get one thread: hwaccel + frame threads is trouble and buys nothing.
        cc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        if (hw != nullptr)
        {
            cc->thread_count = 1;
        }
        else if (opt.decoderThreads > 0)
        {
            cc->thread_count = opt.decoderThreads;
        }
        else
        {
            cc->thread_count = 0; // libavcodec decides (one frame thread per hardware thread, capped at 16)
        }

        // Deliberately no `skip_frame = AVDISCARD_NONKEY` for nearest-keyframe mode: skipped frames
        // do not advance the reorder buffer, so a keyframe only leaves the decoder when the *next*
        // keyframe arrives, which makes a long GOP dramatically slower rather than faster.

        if (hw != nullptr)
        {
            AVBufferRef* dev = nullptr;
            const char* device = opt.hardware.device.empty() ? nullptr : opt.hardware.device.c_str();

            if (int r = av_hwdevice_ctx_create (&dev, hw->type, device, nullptr, 0); r < 0)
            {
                return fail (ErrorCode::hardwareUnavailable, r,
                             std::string ("av_hwdevice_ctx_create(") + av_hwdevice_get_type_name (hw->type)
                                 + (device ? std::string (", \"") + device + "\")" : ")"));
            }

            hwDevice.reset (dev);
            hwType = hw->type;
            hwState = std::make_unique<HwState>();
            hwState->hw_pix_fmt = hw->pix_fmt;
            cc->hw_device_ctx = av_buffer_ref (hwDevice.get());

            if (cc->hw_device_ctx == nullptr) return fail (ErrorCode::outOfMemory, "av_buffer_ref(hw_device_ctx)");
            cc->opaque = hwState.get();
            cc->get_format = &hwGetFormat;
            // Surfaces the pipeline can hold at once beyond the decoder's own needs: held, pending, the
            // corrupt stash and the probe frame.
            cc->extra_hw_frames = 4;
            hwState->extraFrames = 0; // avcodec_get_hw_frames_parameters already adds extra_hw_frames
        }

        if (int r = avcodec_open2 (cc.get(), codecDesc, nullptr); r < 0)
        {
            return fail (hw != nullptr ? ErrorCode::hardwareUnavailable : ErrorCode::decoderOpenFailed, r,
                         std::string ("avcodec_open2(") + codecDesc->name + ")");
        }

        codec = std::move (cc);
        hwActive = hw != nullptr;
        hwFramesSeen = 0;
        return {};
    }

    /// True once a codec context has been opened and not yet replaced by a failed build.
    [[nodiscard]] bool isBuilt() const noexcept { return static_cast<bool> (codec); }

    /// Gives up the codec context and keeps the hardware device and state behind it, so the same
    /// candidate can be rebuilt on top of a re-opened container. Only the re-open path wants this:
    /// it has to be decoderless before it touches the container (see FramePipeline::reopen).
    void releaseContext() noexcept { codec.reset(); }

    /// The decoder's name, as libavcodec reports it. Valid from attach() onwards.
    [[nodiscard]] const char* getCodecName() const noexcept { return codecDesc->name; }

    /// The frame rate the decoder negotiated, {0, x} when it declared none. Only containers that
    /// carry no timestamps of their own have any use for it, and it is the caller that knows that.
    [[nodiscard]] AVRational getFrameRate() const noexcept { return codec ? codec->framerate : AVRational{ 0, 1 }; }

    // ---- decoding ---------------------------------------------------------------------------

    /// Sends one packet. Returns 0 (consumed), k::eagain (the decoder wants a receive first) or an
    /// error. The packet stays the caller's either way.
    [[nodiscard]] int send (AVPacket& pkt) noexcept { return avcodec_send_packet (codec.get(), &pkt); }

    /// Sends the null packet that starts the drain. The result is deliberately ignored: the only
    /// failure libavcodec reports here is "already draining", which is the state being asked for.
    void startDrain() noexcept { (void)avcodec_send_packet (codec.get(), nullptr); }

    /// Pulls one frame into getFrame(). Returns 0, k::eagain (needs a packet), k::eof or an error.
    ///
    /// k::eagain is not translated to k::eof while draining: whether a drain is in progress is the
    /// decode loop's state, not the decoder's, so the loop reads it back from its own frontier.
    [[nodiscard]] int receive() noexcept
    {
        av_frame_unref (recv.get());
        const int r = avcodec_receive_frame (codec.get(), recv.get());

        if (r == 0)
        {
            ++receiveCount;

            if (hwActive) ++hwFramesSeen;
        }

        return r;
    }

    /// The frame the last receive() landed in. Valid from attach() onwards; its contents are
    /// whatever the last receive() (or releaseFrame()) left there.
    [[nodiscard]] AVFrame& getFrame() noexcept { return *recv; }

    /// Drops the received frame's buffers. Called when the caller throws away the decode position:
    /// a hardware surface held here is one the decoder's pool cannot hand out.
    void releaseFrame() noexcept { av_frame_unref (recv.get()); }

    /// Frames the decoder has returned over this VideoDecoder's whole life, across every context it
    /// has built. A monotonic count for the same reason MediaSource counts its own seeks: the
    /// decoder makes the calls, so it is the only place a count cannot drift out of step with them,
    /// and a request is not a scope it knows about. The caller differences it per request.
    [[nodiscard]] std::int64_t getReceiveCount() const noexcept { return receiveCount; }

    /// Drops everything the decoder buffered. Safe before the decoder is built: the paths that
    /// recover from a failed re-open reach here with no context at all.
    void flush() noexcept
    {
        if (codec) avcodec_flush_buffers (codec.get());
    }

    /// Which frames the decoder may drop before it decodes them. The decision is the caller's (it
    /// depends on the request's window and on what has been fed), the field is the decoder's. Only
    /// written when it changes: libavcodec reads it per packet.
    void setSkipPolicy (AVDiscard skip) noexcept
    {
        if (codec->skip_frame != skip) codec->skip_frame = skip;
    }

    // ---- hardware ---------------------------------------------------------------------------

    /// Whether the decoder *last built successfully* was a hardware one. Deliberately not "there is
    /// a working hardware decoder right now": a build that fails leaves this as it was while
    /// isBuilt() goes false, so the two can disagree until the caller re-opens. Every reader wants
    /// the former — which path this decoder is on — and getHardwareCandidate() is what answers
    /// "can it be rebuilt", with the stricter test.
    [[nodiscard]] bool isHardwareActive() const noexcept { return hwActive; }

    /// A hardware decoder error that is not "bad data" (driver/session failure) triggers the
    /// software fallback; corrupt input is handled like any other corruption. A hardware decoder
    /// that fails before producing its first frame is always treated as unusable.
    [[nodiscard]] bool isHardwareFault (int avError) const noexcept
    {
        return hwActive && (hwFramesSeen == 0 || avError != k::invalidData);
    }

    /// The three verbs of the fallback ladder. The fault is raised wherever it is observed (a send,
    /// a receive, a surface download) and consumed by the ladder in FramePipeline::imageAtImpl,
    /// which is two frames up: it is a fact about this decoder, so it lives here rather than in
    /// whichever function happened to see it.
    void noteHardwareFault() noexcept { hwFault = true; }
    [[nodiscard]] bool hasHardwareFault() const noexcept { return hwFault; }
    void clearHardwareFault() noexcept { hwFault = false; }

    /// One hardware rebuild per run of failures; a request that succeeds on hardware re-arms it.
    void noteHardwareRetry() noexcept { hwRetried = true; }
    [[nodiscard]] bool hasRetriedHardware() const noexcept { return hwRetried; }
    void rearmHardwareRetry() noexcept { hwRetried = false; }

    /// The candidate this decoder is currently running, for rebuilding the same one after a fault
    /// or a re-open. Empty unless a hardware context was built and is still the active one.
    [[nodiscard]] std::optional<HwCandidate> getHardwareCandidate() const noexcept
    {
        if (! hwActive || hwState == nullptr || hwDevice == nullptr) return std::nullopt;
        return HwCandidate{ hwType, hwState->hw_pix_fmt };
    }

    /// The pixel format the hardware decoder was asked to produce, AV_PIX_FMT_NONE when there is no
    /// hardware session. The probe compares its frame against it: a software format there means the
    /// decoder quietly fell back and the surfaces we counted on do not exist.
    [[nodiscard]] AVPixelFormat getHardwarePixelFormat() const noexcept
    {
        return hwState != nullptr ? hwState->hw_pix_fmt : AV_PIX_FMT_NONE;
    }

    /// Whether the decoder accepted our hardware format during negotiation (get_format). False
    /// means it offered none — the profile is not supported — which is not an error from
    /// libavcodec's point of view and has to be noticed here.
    [[nodiscard]] bool didAcceptHardwareFormat() const noexcept
    {
        return hwState != nullptr && ! hwState->declined && hwState->gotHwFormat;
    }

    /// HardwarePolicy::automatic: hardware only where it beats multi-threaded software for this
    /// seek-heavy access pattern. Every seek re-initialises the decoder session and every frame is
    /// downloaded, so hardware only pays for the more expensive codecs at larger sizes; software wins
    /// H.264 at every size. The thresholds below were chosen from measurement on the author's machine
    /// — a starting point, not a portable truth; tune them for yours.
    [[nodiscard]] static bool isHardwareWorthwhile (const AVCodecParameters& par, const AVCodec& codec) noexcept
    {
        const std::int64_t pixels = static_cast<std::int64_t> (std::max (par.width, 0)) * std::max (par.height, 0);
        switch (codec.id)
        {
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_AV1:
        case AV_CODEC_ID_VP9:
        case AV_CODEC_ID_VVC:
            return pixels >= static_cast<std::int64_t> (1280) * 720;
        default:
            return false;
        }
    }

    [[nodiscard]] static std::string getHardwareTypeName (AVHWDeviceType t)
    {
        const char* n = av_hwdevice_get_type_name (t);
        return n != nullptr ? std::string{ n } : std::string{ "unknown" };
    }

    // ---- the published snapshot -------------------------------------------------------------

    /// Snapshot of the active decode path. Returned by value and taken under the mutex: this is the
    /// one member any thread may read (see the threading note at the top of this file).
    [[nodiscard]] ActiveDecoder getActiveDecoder() const
    {
        std::lock_guard lk (activeMutex);
        return active;
    }

    /// Publishes "hardware, of the type this decoder was built with". Called from the decode thread
    /// only, once a candidate has been built *and* probed.
    void publishHardware (const std::string& device)
    {
        publish (ActiveDecoder{ codecDesc->name, true, fromAv (hwType), getHardwareTypeName (hwType), device, "" });
    }

    /// Publishes "software, and here is why". `reason` is what the caller wants the user to read in
    /// ActiveDecoder::fallbackReason: the automatic policy's verdict, the hardware attempts that
    /// failed, or a hardware decoder that died mid-run.
    void publishSoftware (std::string reason)
    {
        publish (ActiveDecoder{ codecDesc->name, false, std::nullopt, "", "", std::move (reason) });
    }

private:
    void publish (ActiveDecoder a)
    {
        a.decoderThreads = codec ? codec->thread_count : 0;
        std::lock_guard lk (activeMutex);
        active = std::move (a);
    }

    const AVCodec* codecDesc{ nullptr };
    CodecCtxPtr codec;
    BufferRefPtr hwDevice;
    AVHWDeviceType hwType{ AV_HWDEVICE_TYPE_NONE };
    std::unique_ptr<HwState> hwState;
    bool hwActive{ false };
    int hwFramesSeen{ 0 };
    bool hwFault{ false };
    bool hwRetried{ false }; ///< the hardware decoder was rebuilt for the current run of failures
    FramePtr recv;           ///< receive() writes here; a FrameSlot moves the buffers out of it
    std::int64_t receiveCount{ 0 };
    mutable std::mutex activeMutex;
    ActiveDecoder active; // guarded by activeMutex
};

} // namespace stills::detail

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
// Probing (decoding the first frame to find out whether a candidate really works) is not done here:
// it runs the selection loop against the real source. What is the decoder's own is which hardware
// candidate it is running (getHardwareCandidate), so the owner can rebuild the same one.
//
// Threading. Everything here belongs to the thread running the pipeline's decode loop, with one
// exception: the ActiveDecoder snapshot. getActiveDecoder() is called from other threads while that
// loop runs, so the snapshot is written under activeMutex and read back under it, by value. It is
// the only member another thread may touch.
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
    // Remembers the decoder libavformat picked for the stream and allocates the frame every
    // receive() lands in. Runs after every MediaSource open and re-open; the frame outlives any
    // number of codec contexts built on top of it.
    [[nodiscard]] std::expected<void, Error> attach (const AVCodec* decoderForStream)
    {
        codecDescriptor = decoderForStream;
        auto frame = makeFrame();

        if (! frame) return std::unexpected (frame.error());
        receivedFrame = std::move (*frame);
        return {};
    }

    // Builds a codec context for `par`, over the hardware candidate `hw` (nullptr = software), and
    // opens it. Replaces whatever was built before, hardware session included.
    //
    // On failure the decoder is left unbuilt (isBuilt() is false) but a hardware *device* created
    // on the way may survive, which is what lets a failed candidate still be named in the attempts
    // string. The caller drops everything it decoded from the old context before calling this:
    // nothing decoded by a decoder that no longer exists describes where the new one is.
    [[nodiscard]] std::expected<void, Error> build (const AVCodecParameters& parameters, AVRational timeBase,
                                                    const Options& options, const HwCandidate* candidate)
    {
        hardwareFault = false; // belongs to the decoder being replaced
        codec.reset();
        hardwareDevice.reset();
        hardwareState.reset();

        CodecCtxPtr context{ avcodec_alloc_context3 (codecDescriptor) };

        if (! context) return fail (ErrorCode::outOfMemory, "avcodec_alloc_context3");
        if (int result = avcodec_parameters_to_context (context.get(), &parameters); result < 0)
        {
            return fail (ErrorCode::decoderOpenFailed, result, "avcodec_parameters_to_context");
        }

        // required for best_effort_timestamp / frame->duration
        context->pkt_timebase = timeBase;

        // The same cap inside libavcodec, so a resolution change mid-stream is refused by the decoder
        // rather than allocated for.
        if (options.maxInputPixels) context->max_pixels = *options.maxInputPixels;
        // Frame threading adds latency after every flush but still wins for this seek-heavy workload.
        // Hardware decoders get one thread: hwaccel + frame threads is trouble and buys nothing.
        context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        if (candidate != nullptr)
        {
            context->thread_count = 1;
        }
        else if (options.decoderThreads > 0)
        {
            context->thread_count = options.decoderThreads;
        }
        else
        {
            context->thread_count = 0; // libavcodec decides (one frame thread per hardware thread, capped at 16)
        }

        // Deliberately no `skip_frame = AVDISCARD_NONKEY` for nearest-keyframe mode: skipped frames
        // do not advance the reorder buffer, so a keyframe only leaves the decoder when the *next*
        // keyframe arrives, which makes a long GOP dramatically slower rather than faster.

        if (candidate != nullptr)
        {
            AVBufferRef* deviceHandle = nullptr;
            const char* deviceName = options.hardware.device.empty() ? nullptr : options.hardware.device.c_str();

            if (int result = av_hwdevice_ctx_create (&deviceHandle, candidate->type, deviceName, nullptr, 0);
                result < 0)
            {
                return fail (ErrorCode::hardwareUnavailable, result,
                             std::string ("av_hwdevice_ctx_create(") + av_hwdevice_get_type_name (candidate->type)
                                 + (deviceName != nullptr ? std::string (", \"") + deviceName + "\")" : ")"));
            }

            hardwareDevice.reset (deviceHandle);
            hardwareType = candidate->type;
            hardwareState = std::make_unique<HwState>();
            hardwareState->hardwarePixelFormat = candidate->pixelFormat;
            context->hw_device_ctx = av_buffer_ref (hardwareDevice.get());

            if (context->hw_device_ctx == nullptr) return fail (ErrorCode::outOfMemory, "av_buffer_ref(hw_device_ctx)");
            context->opaque = hardwareState.get();
            context->get_format = &hwGetFormat;
            // Surfaces the pipeline can hold at once beyond the decoder's own needs: held, pending, the
            // corrupt stash and the probe frame.
            context->extra_hw_frames = 4;
            hardwareState->extraFrames = 0; // avcodec_get_hw_frames_parameters already adds extra_hw_frames
        }

        if (int result = avcodec_open2 (context.get(), codecDescriptor, nullptr); result < 0)
        {
            return fail (candidate != nullptr ? ErrorCode::hardwareUnavailable : ErrorCode::decoderOpenFailed, result,
                         std::string ("avcodec_open2(") + codecDescriptor->name + ")");
        }

        codec = std::move (context);
        hardwareActive = candidate != nullptr;
        hardwareFramesSeen = 0;
        return {};
    }

    // True once a codec context has been opened and not yet replaced by a failed build.
    [[nodiscard]] bool isBuilt() const noexcept { return static_cast<bool> (codec); }

    // Gives up the codec context and keeps the hardware device and state behind it, so the same
    // candidate can be rebuilt on top of a re-opened container. Only the re-open path wants this:
    // it has to be decoderless before it touches the container (see FramePipeline::reopen).
    void releaseContext() noexcept { codec.reset(); }

    // The decoder's name, as libavcodec reports it. Valid from attach() onwards.
    [[nodiscard]] const char* getCodecName() const noexcept { return codecDescriptor->name; }

    // The frame rate the decoder negotiated, {0, x} when it declared none. Only containers that
    // carry no timestamps of their own have any use for it, and it is the caller that knows that.
    [[nodiscard]] AVRational getFrameRate() const noexcept { return codec ? codec->framerate : AVRational{ 0, 1 }; }

    // ---- decoding ---------------------------------------------------------------------------

    // Sends one packet. Returns 0 (consumed), libav::eagain (the decoder wants a receive first) or an
    // error. The packet stays the caller's either way.
    [[nodiscard]] int send (AVPacket& packet) noexcept { return avcodec_send_packet (codec.get(), &packet); }

    // Sends the null packet that starts the drain. The result is deliberately ignored: the only
    // failure libavcodec reports here is "already draining", which is the state being asked for.
    void startDrain() noexcept { (void)avcodec_send_packet (codec.get(), nullptr); }

    // Pulls one frame into getFrame(). Returns 0, libav::eagain (needs a packet), libav::eof or an error.
    //
    // libav::eagain is not translated to libav::eof while draining: whether a drain is in progress is the
    // decode loop's state, not the decoder's, so the loop reads it back from its own frontier.
    [[nodiscard]] int receive() noexcept
    {
        av_frame_unref (receivedFrame.get());
        const int result = avcodec_receive_frame (codec.get(), receivedFrame.get());

        if (result == 0)
        {
            ++receiveCount;

            if (hardwareActive) ++hardwareFramesSeen;
        }

        return result;
    }

    // The frame the last receive() landed in. Valid from attach() onwards; its contents are
    // whatever the last receive() (or releaseFrame()) left there.
    [[nodiscard]] AVFrame& getFrame() noexcept { return *receivedFrame; }

    // Drops the received frame's buffers. Called when the caller throws away the decode position:
    // a hardware surface held here is one the decoder's pool cannot hand out.
    void releaseFrame() noexcept { av_frame_unref (receivedFrame.get()); }

    // Frames the decoder has returned over this VideoDecoder's whole life, across every context it
    // has built. A monotonic count for the same reason MediaSource counts its own seeks: the
    // decoder makes the calls, so it is the only place a count cannot drift out of step with them,
    // and a request is not a scope it knows about. The caller differences it per request.
    [[nodiscard]] std::int64_t getReceiveCount() const noexcept { return receiveCount; }

    // Drops everything the decoder buffered. Safe before the decoder is built: the paths that
    // recover from a failed re-open reach here with no context at all.
    void flush() noexcept
    {
        if (codec) avcodec_flush_buffers (codec.get());
    }

    // Which frames the decoder may drop before it decodes them. The decision is the caller's (it
    // depends on the request's window and on what has been fed), the field is the decoder's. Only
    // written when it changes: libavcodec reads it per packet.
    void setSkipPolicy (AVDiscard skip) noexcept
    {
        if (codec->skip_frame != skip) codec->skip_frame = skip;
    }

    // ---- hardware ---------------------------------------------------------------------------

    // Whether the decoder *last built successfully* was a hardware one. Deliberately not "there is
    // a working hardware decoder right now": a build that fails leaves this as it was while
    // isBuilt() goes false, so the two can disagree until the caller re-opens. Every reader wants
    // the former — which path this decoder is on — and getHardwareCandidate() is what answers
    // "can it be rebuilt", with the stricter test.
    [[nodiscard]] bool isHardwareActive() const noexcept { return hardwareActive; }

    // A hardware decoder error that is not "bad data" (driver/session failure) triggers the
    // software fallback; corrupt input is handled like any other corruption. A hardware decoder
    // that fails before producing its first frame is always treated as unusable.
    [[nodiscard]] bool isHardwareFault (int avError) const noexcept
    {
        return hardwareActive && (hardwareFramesSeen == 0 || avError != libav::invalidData);
    }

    // The three verbs of the fallback ladder. The fault is raised wherever it is observed (a send,
    // a receive, a surface download) and consumed by the ladder in FramePipeline::imageAtImpl,
    // which is two frames up: it is a fact about this decoder, so it lives here rather than in
    // whichever function happened to see it.
    void noteHardwareFault() noexcept { hardwareFault = true; }
    [[nodiscard]] bool hasHardwareFault() const noexcept { return hardwareFault; }
    void clearHardwareFault() noexcept { hardwareFault = false; }

    // One hardware rebuild per run of failures; a request that succeeds on hardware re-arms it.
    void noteHardwareRetry() noexcept { hardwareRetried = true; }
    [[nodiscard]] bool hasRetriedHardware() const noexcept { return hardwareRetried; }
    void rearmHardwareRetry() noexcept { hardwareRetried = false; }

    // The candidate this decoder is currently running, for rebuilding the same one after a fault
    // or a re-open. Empty unless a hardware context was built and is still the active one.
    [[nodiscard]] std::optional<HwCandidate> getHardwareCandidate() const noexcept
    {
        if (! hardwareActive || hardwareState == nullptr || hardwareDevice == nullptr) return std::nullopt;
        return HwCandidate{ hardwareType, hardwareState->hardwarePixelFormat };
    }

    // The pixel format the hardware decoder was asked to produce, AV_PIX_FMT_NONE when there is no
    // hardware session. The probe compares its frame against it: a software format there means the
    // decoder quietly fell back and the surfaces we counted on do not exist.
    [[nodiscard]] AVPixelFormat getHardwarePixelFormat() const noexcept
    {
        return hardwareState != nullptr ? hardwareState->hardwarePixelFormat : AV_PIX_FMT_NONE;
    }

    // Whether the decoder accepted our hardware format during negotiation (get_format). False
    // means it offered none — the profile is not supported — which is not an error from
    // libavcodec's point of view and has to be noticed here.
    [[nodiscard]] bool didAcceptHardwareFormat() const noexcept
    {
        return hardwareState != nullptr && ! hardwareState->declined && hardwareState->gotHwFormat;
    }

    // HardwarePolicy::automatic as it stands: a lookup made once at open(), with no measurement
    // behind it. HEVC/AV1/VP9/VVC at 720p and up go to hardware, everything else to software.
    //
    // The rule is known to be wrong in both directions. It was
    // fitted to one machine's NVDEC pathology: the h264 decoder renegotiates its pixel format after
    // every avcodec_flush_buffers() — once per seek, and seeking is this library's core operation —
    // and NVDEC rebuilds its CUVID decoder each time, which made software 4.7x faster there.
    // Generalising that to a codec rule predicts wrongly in both directions elsewhere. On VAAPI
    // hardware wins H.264 at every size by 1.3-1.9x. On VideoToolbox software wins H.264, but for an
    // unrelated reason: unified memory makes the CPU decoder fast and already leaves the frame where
    // the converter wants it, while the GPU path pays session setup per flush plus a surface
    // download per frame. So the table is right on VideoToolbox by accident and wrong on VAAPI.
    //
    // Counting get_format renegotiations would not fix it. A count is a frequency, not a cost: H.264
    // renegotiates once per seek on every device, and the same count is a cheap context re-init on
    // VAAPI and a decoder rebuild on NVDEC — roughly ten times the cost, flagged identically. And
    // VideoToolbox's problem is not renegotiation at all, so a counter would never fire on the one
    // machine that most needs it to.
    //
    // The signal that separates all three is already collected: SeekCostModel splits seek cost
    // (positioning plus the first frame out of a flushed decoder) from per-frame cost.
    // Renegotiation and session setup land in the first, surface download in the second, and both
    // are measured on the caller's content on the caller's machine. The design that follows — this
    // table as an initial guess only, then one A/B on real content per (codec, resolution class,
    // device type) per process, the winner cached and the numbers reported in
    // ActiveDecoder::fallbackReason — is written up in the README under "What `automatic` should
    // be". Deliberately not implemented here: an adaptive policy is a feature, not a fix.
    //
    // A renegotiation counter still earns a place, as a diagnostic on ActiveDecoder: it is what
    // explains a bad hardware number to whoever reads the log. Not as the decision input.
    [[nodiscard]] static bool isHardwareWorthwhile (const AVCodecParameters& parameters, const AVCodec& codec) noexcept
    {
        const std::int64_t pixels =
            static_cast<std::int64_t> (std::max (parameters.width, 0)) * std::max (parameters.height, 0);
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

    [[nodiscard]] static std::string getHardwareTypeName (AVHWDeviceType type)
    {
        const char* name = av_hwdevice_get_type_name (type);
        return name != nullptr ? std::string{ name } : std::string{ "unknown" };
    }

    // ---- the published snapshot -------------------------------------------------------------

    // Snapshot of the active decode path. Returned by value and taken under the mutex: this is the
    // one member any thread may read (see the threading note at the top of this file).
    [[nodiscard]] ActiveDecoder getActiveDecoder() const
    {
        std::lock_guard lock (activeMutex);
        return active;
    }

    // Publishes "hardware, of the type this decoder was built with". Called from the decode thread
    // only, once a candidate has been built *and* probed.
    void publishHardware (const std::string& device)
    {
        publish (ActiveDecoder{ codecDescriptor->name, true, fromAv (hardwareType), getHardwareTypeName (hardwareType),
                                device, "" });
    }

    // Publishes "software, and here is why". `reason` is what the caller wants the user to read in
    // ActiveDecoder::fallbackReason: the automatic policy's verdict, the hardware attempts that
    // failed, or a hardware decoder that died mid-run.
    void publishSoftware (std::string reason)
    {
        publish (ActiveDecoder{ codecDescriptor->name, false, std::nullopt, "", "", std::move (reason) });
    }

private:
    void publish (ActiveDecoder snapshot)
    {
        snapshot.decoderThreads = codec ? codec->thread_count : 0;
        std::lock_guard lock (activeMutex);
        active = std::move (snapshot);
    }

    const AVCodec* codecDescriptor{ nullptr };
    CodecCtxPtr codec;
    BufferRefPtr hardwareDevice;
    AVHWDeviceType hardwareType{ AV_HWDEVICE_TYPE_NONE };
    std::unique_ptr<HwState> hardwareState;
    bool hardwareActive{ false };
    int hardwareFramesSeen{ 0 };
    bool hardwareFault{ false };
    bool hardwareRetried{ false }; // the hardware decoder was rebuilt for the current run of failures
    FramePtr receivedFrame;        // receive() writes here; a FrameSlot moves the buffers out of it
    std::int64_t receiveCount{ 0 };
    mutable std::mutex activeMutex;
    ActiveDecoder active; // guarded by activeMutex
};

} // namespace stills::detail

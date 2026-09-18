#pragma once
// stills/detail/stills_HardwareSupport.h — hardware decoder candidate enumeration and the
// get_format negotiation.

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "stills/detail/stills_FFmpeg.h"
#include "stills/stills_Options.h"

namespace stills::detail
{

// Shared with libavcodec through AVCodecContext::opaque. Heap-allocated by the pipeline so its
// address is stable for the codec context's lifetime.
struct HwState
{
    AVPixelFormat hardwarePixelFormat{ AV_PIX_FMT_NONE };
    bool gotHwFormat{ false }; // the decoder accepted our hardware format at least once
    bool declined{ false };    // the decoder offered no hardware format (profile unsupported)
    // The surface pool, allocated once and handed to the decoder again after every flush (the
    // h264/hevc decoders renegotiate the format after avcodec_flush_buffers and libavcodec drops
    // the previous pool). Re-created when the coded size or software format changes mid-stream.
    BufferRefPtr framesCtx;
    int framesWidth{ 0 }, framesHeight{ 0 };
    AVPixelFormat framesSwFmt{ AV_PIX_FMT_NONE };
    int extraFrames{ 0 }; // surfaces beyond the decoder's own needs (the pipeline's held frames)
};

// Supplies the persistent pool to the decoder (allocating it on first use or after a geometry
// change). Failure is not fatal: libavcodec allocates its own pool per negotiation as before.
inline void hwSupplyFramesCtx (AVCodecContext* context, HwState& state) noexcept
{
    if (context->hw_device_ctx == nullptr) return;
    const int width = context->coded_width > 0 ? context->coded_width : context->width;
    const int height = context->coded_height > 0 ? context->coded_height : context->height;

    if (state.framesCtx
        && (state.framesWidth != width || state.framesHeight != height || state.framesSwFmt != context->sw_pix_fmt))
        state.framesCtx.reset();

    if (! state.framesCtx)
    {
        AVBufferRef* reference = nullptr;

        if (avcodec_get_hw_frames_parameters (context, context->hw_device_ctx, state.hardwarePixelFormat, &reference)
                < 0
            || reference == nullptr)
            return;
        auto* framesContext = reinterpret_cast<AVHWFramesContext*> (reference->data);

        // libavcodec's own ff_decode_get_hw_frames_ctx guarantees 4 base work surfaces on top of what
        // avcodec_get_hw_frames_parameters asks for (which guarantees 1): add the same 3.
        if (framesContext->initial_pool_size > 0) framesContext->initial_pool_size += state.extraFrames + 3;
        if (av_hwframe_ctx_init (reference) < 0)
        {
            av_buffer_unref (&reference);
            return;
        }

        state.framesCtx.reset (reference);
        state.framesWidth = width;
        state.framesHeight = height;
        state.framesSwFmt = context->sw_pix_fmt;
    }

    if (context->hw_frames_ctx == nullptr) context->hw_frames_ctx = av_buffer_ref (state.framesCtx.get());
}

// AVCodecContext::get_format callback: pick the negotiated hardware format if offered, else the
// first software format (and remember that hardware was declined).
inline AVPixelFormat hwGetFormat (AVCodecContext* context, const AVPixelFormat* offered) noexcept
{
    auto* state = static_cast<HwState*> (context->opaque);

    for (const AVPixelFormat* format = offered; *format != AV_PIX_FMT_NONE; ++format)
    {
        if (state != nullptr && *format == state->hardwarePixelFormat)
        {
            state->gotHwFormat = true;
            hwSupplyFramesCtx (context, *state);
            return *format;
        }
    }

    if (state != nullptr) state->declined = true;
    for (const AVPixelFormat* format = offered; *format != AV_PIX_FMT_NONE; ++format)
    {
        const auto* descriptor = av_pix_fmt_desc_get (*format);

        if (descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) return *format;
    }

    return offered[0];
}

struct HwCandidate
{
    AVHWDeviceType type{ AV_HWDEVICE_TYPE_NONE };
    AVPixelFormat pixelFormat{ AV_PIX_FMT_NONE };
};

// Static preference order (first match wins), by device family. Anything not listed (or not known
// to this FFmpeg build) keeps libav's order after the listed ones.
// CUDA/NVDEC is deliberately late: the h264 decoder renegotiates the hardware format after every
// avcodec_flush_buffers() — one get_format call per seek — and NVDEC re-creates its decoder each
// time, which dominates a seek. HEVC on NVDEC does not renegotiate. Seeking is this library's core
// operation, so the order reflects that; callers who want NVDEC anyway select it with
// Options::hardware.deviceType. The order was chosen from measurement on one machine.
inline constexpr std::array<HardwareDeviceType, 11> hwPreference{
    HardwareDeviceType::videotoolbox, HardwareDeviceType::vaapi,     HardwareDeviceType::d3d11va,
    HardwareDeviceType::dxva2,        HardwareDeviceType::qsv,       HardwareDeviceType::vdpau,
    HardwareDeviceType::cuda,         HardwareDeviceType::vulkan,    HardwareDeviceType::opencl,
    HardwareDeviceType::drm,          HardwareDeviceType::mediacodec
};

[[nodiscard]] inline int hwRank (AVHWDeviceType type) noexcept
{
    const std::optional<HardwareDeviceType> known = fromAv (type);

    if (! known) return static_cast<int> (hwPreference.size());
    for (std::size_t index = 0; index < hwPreference.size(); ++index)
    {
        if (hwPreference[index] == *known) return static_cast<int> (index);
    }

    return static_cast<int> (hwPreference.size());
}

// Hardware device types this decoder can use through a device context, filtered by Options and
// ordered by preference. `reason` explains an empty result.
[[nodiscard]] inline std::vector<HwCandidate> hwCandidates (const AVCodec* codec, const Options& opt,
                                                            std::string& reason)
{
    std::vector<HwCandidate> candidates;

    if (opt.hardware.policy == HardwarePolicy::softwareOnly)
    {
        reason = "softwareOnly policy";
        return candidates;
    }

    AVHWDeviceType wanted = AV_HWDEVICE_TYPE_NONE;

    if (opt.hardware.deviceType)
    {
        wanted = toAv (*opt.hardware.deviceType);

        if (wanted == AV_HWDEVICE_TYPE_NONE)
        {
            reason = std::string{ "this FFmpeg build has no hardware device type '" }
                     + std::string{ toString (*opt.hardware.deviceType) } + "'";
            return candidates;
        }
    }

    for (int index = 0;; ++index)
    {
        const AVCodecHWConfig* config = avcodec_get_hw_config (codec, index);

        if (config == nullptr) break;
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0) continue;
        if (wanted != AV_HWDEVICE_TYPE_NONE && config->device_type != wanted) continue;
        const bool alreadyListed = std::any_of (candidates.begin(), candidates.end(),
                                                [&] (const HwCandidate& c) { return c.type == config->device_type; });

        if (! alreadyListed) candidates.push_back ({ config->device_type, config->pix_fmt });
    }

    if (candidates.empty())
    {
        reason = std::string{ "decoder '" } + codec->name;

        if (wanted != AV_HWDEVICE_TYPE_NONE)
            reason += std::string{ "' does not support device type '" } + av_hwdevice_get_type_name (wanted) + "'";
        else
            reason += "' has no hardware device configurations";
        return candidates;
    }

    std::stable_sort (candidates.begin(), candidates.end(),
                      [] (const HwCandidate& a, const HwCandidate& b) { return hwRank (a.type) < hwRank (b.type); });
    return candidates;
}

// Device types that can actually be created on this machine (default device string).
[[nodiscard]] inline std::vector<HardwareDeviceType> probeAvailableHwTypes()
{
    std::vector<HardwareDeviceType> candidates;

    for (AVHWDeviceType type = av_hwdevice_iterate_types (AV_HWDEVICE_TYPE_NONE); type != AV_HWDEVICE_TYPE_NONE;
         type = av_hwdevice_iterate_types (type))
    {
        AVBufferRef* reference = nullptr;

        if (av_hwdevice_ctx_create (&reference, type, nullptr, nullptr, 0) == 0)
        {
            BufferRefPtr ownedReference{ reference };

            if (auto known = fromAv (type)) candidates.push_back (*known);
        }
    }

    return candidates;
}

} // namespace stills::detail

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
    AVPixelFormat hw_pix_fmt{ AV_PIX_FMT_NONE };
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
inline void hwSupplyFramesCtx (AVCodecContext* cc, HwState& st) noexcept
{
    if (cc->hw_device_ctx == nullptr) return;
    const int w = cc->coded_width > 0 ? cc->coded_width : cc->width;
    const int h = cc->coded_height > 0 ? cc->coded_height : cc->height;

    if (st.framesCtx && (st.framesWidth != w || st.framesHeight != h || st.framesSwFmt != cc->sw_pix_fmt))
        st.framesCtx.reset();

    if (! st.framesCtx)
    {
        AVBufferRef* ref = nullptr;

        if (avcodec_get_hw_frames_parameters (cc, cc->hw_device_ctx, st.hw_pix_fmt, &ref) < 0 || ref == nullptr) return;
        auto* fc = reinterpret_cast<AVHWFramesContext*> (ref->data);

        // libavcodec's own ff_decode_get_hw_frames_ctx guarantees 4 base work surfaces on top of what
        // avcodec_get_hw_frames_parameters asks for (which guarantees 1): add the same 3.
        if (fc->initial_pool_size > 0) fc->initial_pool_size += st.extraFrames + 3;
        if (av_hwframe_ctx_init (ref) < 0)
        {
            av_buffer_unref (&ref);
            return;
        }

        st.framesCtx.reset (ref);
        st.framesWidth = w;
        st.framesHeight = h;
        st.framesSwFmt = cc->sw_pix_fmt;
    }

    if (cc->hw_frames_ctx == nullptr) cc->hw_frames_ctx = av_buffer_ref (st.framesCtx.get());
}

// AVCodecContext::get_format callback: pick the negotiated hardware format if offered, else the
// first software format (and remember that hardware was declined).
inline AVPixelFormat hwGetFormat (AVCodecContext* cc, const AVPixelFormat* fmts) noexcept
{
    auto* st = static_cast<HwState*> (cc->opaque);

    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
    {
        if (st != nullptr && *p == st->hw_pix_fmt)
        {
            st->gotHwFormat = true;
            hwSupplyFramesCtx (cc, *st);
            return *p;
        }
    }

    if (st != nullptr) st->declined = true;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
    {
        const auto* desc = av_pix_fmt_desc_get (*p);

        if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) return *p;
    }

    return fmts[0];
}

struct HwCandidate
{
    AVHWDeviceType type{ AV_HWDEVICE_TYPE_NONE };
    AVPixelFormat pix_fmt{ AV_PIX_FMT_NONE };
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

[[nodiscard]] inline int hwRank (AVHWDeviceType t) noexcept
{
    const std::optional<HardwareDeviceType> ours = fromAv (t);

    if (! ours) return static_cast<int> (hwPreference.size());
    for (std::size_t i = 0; i < hwPreference.size(); ++i)
    {
        if (hwPreference[i] == *ours) return static_cast<int> (i);
    }

    return static_cast<int> (hwPreference.size());
}

// Hardware device types this decoder can use through a device context, filtered by Options and
// ordered by preference. `reason` explains an empty result.
[[nodiscard]] inline std::vector<HwCandidate> hwCandidates (const AVCodec* codec, const Options& opt,
                                                            std::string& reason)
{
    std::vector<HwCandidate> out;

    if (opt.hardware.policy == HardwarePolicy::softwareOnly)
    {
        reason = "softwareOnly policy";
        return out;
    }

    AVHWDeviceType wanted = AV_HWDEVICE_TYPE_NONE;

    if (opt.hardware.deviceType)
    {
        wanted = toAv (*opt.hardware.deviceType);

        if (wanted == AV_HWDEVICE_TYPE_NONE)
        {
            reason = std::string{ "this FFmpeg build has no hardware device type '" }
                     + std::string{ toString (*opt.hardware.deviceType) } + "'";
            return out;
        }
    }

    for (int i = 0;; ++i)
    {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config (codec, i);

        if (cfg == nullptr) break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0) continue;
        if (wanted != AV_HWDEVICE_TYPE_NONE && cfg->device_type != wanted) continue;
        const bool dup =
            std::any_of (out.begin(), out.end(), [&] (const HwCandidate& c) { return c.type == cfg->device_type; });

        if (! dup) out.push_back ({ cfg->device_type, cfg->pix_fmt });
    }

    if (out.empty())
    {
        reason = std::string{ "decoder '" } + codec->name;

        if (wanted != AV_HWDEVICE_TYPE_NONE)
            reason += std::string{ "' does not support device type '" } + av_hwdevice_get_type_name (wanted) + "'";
        else
            reason += "' has no hardware device configurations";
        return out;
    }

    std::stable_sort (out.begin(), out.end(),
                      [] (const HwCandidate& a, const HwCandidate& b) { return hwRank (a.type) < hwRank (b.type); });
    return out;
}

// Device types that can actually be created on this machine (default device string).
[[nodiscard]] inline std::vector<HardwareDeviceType> probeAvailableHwTypes()
{
    std::vector<HardwareDeviceType> out;

    for (AVHWDeviceType t = av_hwdevice_iterate_types (AV_HWDEVICE_TYPE_NONE); t != AV_HWDEVICE_TYPE_NONE;
         t = av_hwdevice_iterate_types (t))
    {
        AVBufferRef* ref = nullptr;

        if (av_hwdevice_ctx_create (&ref, t, nullptr, nullptr, 0) == 0)
        {
            BufferRefPtr owned{ ref };

            if (auto h = fromAv (t)) out.push_back (*h);
        }
    }

    return out;
}

} // namespace stills::detail

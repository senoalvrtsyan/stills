#pragma once
// stills/detail/hw.hpp — hardware decoder candidate enumeration and the get_format negotiation.

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

#include "stills/detail/ffmpeg.hpp"
#include "stills/options.hpp"

namespace stills::detail {

/// Shared with libavcodec through AVCodecContext::opaque. Heap-allocated by the pipeline so its
/// address is stable for the codec context's lifetime.
struct HwState {
  AVPixelFormat hw_pix_fmt{AV_PIX_FMT_NONE};
  bool got_hw_format{false};  ///< the decoder accepted our hardware format at least once
  bool declined{false};       ///< the decoder offered no hardware format (profile unsupported)
  /// The surface pool, allocated once and handed to the decoder again after every flush (the
  /// h264/hevc decoders renegotiate the format after avcodec_flush_buffers and libavcodec drops
  /// the previous pool). Re-created when the coded size or software format changes mid-stream.
  BufferRefPtr frames_ctx;
  int frames_width{0}, frames_height{0};
  AVPixelFormat frames_sw_fmt{AV_PIX_FMT_NONE};
  int extra_frames{0};  ///< surfaces beyond the decoder's own needs (the pipeline's held frames)
};

/// Supplies the persistent pool to the decoder (allocating it on first use or after a geometry
/// change). Failure is not fatal: libavcodec allocates its own pool per negotiation as before.
inline void hw_supply_frames_ctx(AVCodecContext* cc, HwState& st) noexcept {
  if (cc->hw_device_ctx == nullptr) return;
  const int w = cc->coded_width > 0 ? cc->coded_width : cc->width;
  const int h = cc->coded_height > 0 ? cc->coded_height : cc->height;
  if (st.frames_ctx &&
      (st.frames_width != w || st.frames_height != h || st.frames_sw_fmt != cc->sw_pix_fmt))
    st.frames_ctx.reset();
  if (!st.frames_ctx) {
    AVBufferRef* ref = nullptr;
    if (avcodec_get_hw_frames_parameters(cc, cc->hw_device_ctx, st.hw_pix_fmt, &ref) < 0 ||
        ref == nullptr)
      return;
    auto* fc = reinterpret_cast<AVHWFramesContext*>(ref->data);
    // libavcodec's own ff_decode_get_hw_frames_ctx guarantees 4 base work surfaces on top of what
    // avcodec_get_hw_frames_parameters asks for (which guarantees 1): add the same 3.
    if (fc->initial_pool_size > 0) fc->initial_pool_size += st.extra_frames + 3;
    if (av_hwframe_ctx_init(ref) < 0) {
      av_buffer_unref(&ref);
      return;
    }
    st.frames_ctx.reset(ref);
    st.frames_width = w;
    st.frames_height = h;
    st.frames_sw_fmt = cc->sw_pix_fmt;
  }
  if (cc->hw_frames_ctx == nullptr) cc->hw_frames_ctx = av_buffer_ref(st.frames_ctx.get());
}

/// AVCodecContext::get_format callback: pick the negotiated hardware format if offered, else the
/// first software format (and remember that hardware was declined).
inline AVPixelFormat hw_get_format(AVCodecContext* cc, const AVPixelFormat* fmts) noexcept {
  auto* st = static_cast<HwState*>(cc->opaque);
  for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (st != nullptr && *p == st->hw_pix_fmt) {
      st->got_hw_format = true;
      hw_supply_frames_ctx(cc, *st);
      return *p;
    }
  }
  if (st != nullptr) st->declined = true;
  for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
    const auto* desc = av_pix_fmt_desc_get(*p);
    if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) return *p;
  }
  return fmts[0];
}

struct HwCandidate {
  AVHWDeviceType type{AV_HWDEVICE_TYPE_NONE};
  AVPixelFormat pix_fmt{AV_PIX_FMT_NONE};
};

/// Static preference order (first match wins), by device family. Anything not listed (or not known
/// to this FFmpeg build) keeps libav's order after the listed ones.
/// CUDA/NVDEC is deliberately late: the h264 decoder renegotiates the hardware format after every
/// avcodec_flush_buffers() — one get_format call per seek — and NVDEC re-creates its decoder each
/// time, which dominates a seek. HEVC on NVDEC does not renegotiate. Seeking is this library's core
/// operation, so the order reflects that; callers who want NVDEC anyway select it with
/// Options::hardware.device_type. The order was chosen from measurement on one machine.
inline constexpr std::array<HardwareDeviceType, 11> hw_preference{
    HardwareDeviceType::videotoolbox, HardwareDeviceType::vaapi,     HardwareDeviceType::d3d11va,
    HardwareDeviceType::dxva2,        HardwareDeviceType::qsv,       HardwareDeviceType::vdpau,
    HardwareDeviceType::cuda,         HardwareDeviceType::vulkan,    HardwareDeviceType::opencl,
    HardwareDeviceType::drm,          HardwareDeviceType::mediacodec};

[[nodiscard]] inline int hw_rank(AVHWDeviceType t) noexcept {
  const std::optional<HardwareDeviceType> ours = from_av(t);
  if (!ours) return static_cast<int>(hw_preference.size());
  for (std::size_t i = 0; i < hw_preference.size(); ++i) {
    if (hw_preference[i] == *ours) return static_cast<int>(i);
  }
  return static_cast<int>(hw_preference.size());
}

/// Hardware device types this decoder can use through a device context, filtered by Options and
/// ordered by preference. `reason` explains an empty result.
[[nodiscard]] inline std::vector<HwCandidate> hw_candidates(const AVCodec* codec,
                                                            const Options& opt,
                                                            std::string& reason) {
  std::vector<HwCandidate> out;
  if (opt.hardware.policy == HardwarePolicy::software_only) {
    reason = "software_only policy";
    return out;
  }
  AVHWDeviceType wanted = AV_HWDEVICE_TYPE_NONE;
  if (opt.hardware.device_type) {
    wanted = to_av(*opt.hardware.device_type);
    if (wanted == AV_HWDEVICE_TYPE_NONE) {
      reason = std::string{"this FFmpeg build has no hardware device type '"} +
               std::string{to_string(*opt.hardware.device_type)} + "'";
      return out;
    }
  }
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
    if (cfg == nullptr) break;
    if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0) continue;
    if (wanted != AV_HWDEVICE_TYPE_NONE && cfg->device_type != wanted) continue;
    const bool dup = std::any_of(out.begin(), out.end(),
                                 [&](const HwCandidate& c) { return c.type == cfg->device_type; });
    if (!dup) out.push_back({cfg->device_type, cfg->pix_fmt});
  }
  if (out.empty()) {
    reason =
        wanted != AV_HWDEVICE_TYPE_NONE
            ? std::string{"decoder '"} + codec->name + "' does not support device type '" +
                  av_hwdevice_get_type_name(wanted) + "'"
            : std::string{"decoder '"} + codec->name + "' has no hardware device configurations";
    return out;
  }
  std::stable_sort(out.begin(), out.end(), [](const HwCandidate& a, const HwCandidate& b) {
    return hw_rank(a.type) < hw_rank(b.type);
  });
  return out;
}

/// Device types that can actually be created on this machine (default device string).
[[nodiscard]] inline std::vector<HardwareDeviceType> probe_available_hw_types() {
  std::vector<HardwareDeviceType> out;
  for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
       t != AV_HWDEVICE_TYPE_NONE; t = av_hwdevice_iterate_types(t)) {
    AVBufferRef* ref = nullptr;
    if (av_hwdevice_ctx_create(&ref, t, nullptr, nullptr, 0) == 0) {
      BufferRefPtr owned{ref};
      if (auto h = from_av(t)) out.push_back(*h);
    }
  }
  return out;
}

}  // namespace stills::detail

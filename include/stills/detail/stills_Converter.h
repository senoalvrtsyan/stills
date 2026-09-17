#pragma once
// stills/detail/stills_Converter.h — output geometry (SAR, rotation, fit-within, even rule), swscale
// conversion with correct colour matrix/range, and hardware->software transfer.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

#include "stills/detail/stills_DecoderHooks.h"
#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_Transform.h"
#include "stills/stills_Geometry.h"
#include "stills/stills_Options.h"
#include "stills/stills_PixelFormat.h"

namespace stills::detail
{

// Display size of a coded frame: sample-aspect-ratio corrected (when enabled) and rotated.
[[nodiscard]] inline Size displaySize (Size coded, AVRational sar, bool applySar, int rotation) noexcept
{
    Size d = coded;

    if (applySar && sar.num > 0 && sar.den > 0 && sar.num != sar.den)
    {
        const double w = static_cast<double> (coded.width) * sar.num / sar.den;
        d.width = std::max (1, static_cast<int> (std::lround (w)));
    }

    if (rotation == 90 || rotation == 270) d = d.transposed();
    return d;
}

// Fits `display` into `max` (either dimension 0/absent = unconstrained) preserving aspect ratio,
// never upscaling, and rounding down to even for chroma-subsampled formats (minimum 2x2; boxes
// smaller than that are rejected by Options::validate).
[[nodiscard]] inline Size fitSize (Size display, std::optional<Size> max, PixelFormat fmt) noexcept
{
    double scale = 1.0;

    if (max)
    {
        if (max->width > 0) scale = std::min (scale, static_cast<double> (max->width) / display.width);
        if (max->height > 0) scale = std::min (scale, static_cast<double> (max->height) / display.height);
    }

    Size out{ std::max (1, static_cast<int> (std::lround (display.width * scale))),
              std::max (1, static_cast<int> (std::lround (display.height * scale))) };

    if (hasChromaSubsampling (fmt))
    {
        out.width = std::max (2, out.width & ~1);
        out.height = std::max (2, out.height & ~1);
    }

    return out;
}

// Resolves the sample aspect ratio the way av_guess_sample_aspect_ratio (and ffmpeg/ffplay) do:
// the container's declaration wins, then the frame's, then the codec's; 1:1 when nothing is set.
[[nodiscard]] inline AVRational resolveSar (AVRational container, AVRational frame, AVRational codec) noexcept
{
    const auto ok = [] (AVRational r) { return r.num > 0 && r.den > 0; };

    if (ok (container)) return container;
    if (ok (frame)) return frame;
    if (ok (codec)) return codec;
    return AVRational{ 1, 1 };
}

[[nodiscard]] inline std::expected<FramePtr, Error> transferToSoftwareUnpooled (const AVFrame& hw);

// Converts decoded software frames to the configured output format/size.
class Converter
{
public:
    // `stripDisplayMatrix`: the display transform has been applied, so the output frame must not
    // carry the AV_FRAME_DATA_DISPLAYMATRIX side data any more (an interop consumer honouring it
    // would rotate a second time).
    Converter (PixelFormat fmt, Scaler scaler, std::optional<Size> max, bool applySar, int rotation, bool mirror,
               bool stripDisplayMatrix) noexcept
      : fmt (fmt),
        // SWS_ACCURATE_RND is deliberately not set: it disables swscale's fast YUV->RGB paths for
        // at most one LSB. SWS_FULL_CHR_H_INT (full chroma interpolation for RGB outputs) is free
        // on those paths.
        swsFlags (toSwsFlags (scaler) | SWS_FULL_CHR_H_INT), maxSize (max), applySar (applySar), rotation (rotation),
        mirror (mirror), stripDisplayMatrix (stripDisplayMatrix)
    {
    }

    [[nodiscard]] PixelFormat getPixelFormat() const noexcept { return fmt; }
    [[nodiscard]] int getRotation() const noexcept { return rotation; }
    [[nodiscard]] bool isMirrored() const noexcept { return mirror; }

    // Final (rotated) output size for a source of this coded size and SAR.
    [[nodiscard]] Size outputSize (Size coded, AVRational sar) const noexcept
    {
        return fitSize (displaySize (coded, sar, applySar, rotation), maxSize, fmt);
    }

    // Downloads a hardware frame into a pooled software frame (props copied). Pooling matters: a
    // fresh allocation per transfer is large enough for glibc to mmap it, so every transfer pays for
    // page faults on top of the DMA.
    [[nodiscard]] std::expected<FramePtr, Error> download (const AVFrame& hw)
    {
        AVPixelFormat swFmt = AV_PIX_FMT_NONE;

        if (hw.hw_frames_ctx != nullptr)
            swFmt = reinterpret_cast<const AVHWFramesContext*> (hw.hw_frames_ctx->data)->sw_format;

        if (swFmt == AV_PIX_FMT_NONE) return transferToSoftwareUnpooled (hw);
        auto sw = pooledFrame (swFmt, hw.width, hw.height, "download");

        if (! sw) return std::unexpected (sw.error());
        // Through the hook (detail/stills_DecoderHooks.h) so the rebuild-once path a driver fault
        // drives can be tested on the shipped code. Defaults to av_hwframe_transfer_data itself.
        if (int r = decoderHooks().hwframeTransferData (sw->get(), &hw, 0); r < 0)
        {
            return fail (ErrorCode::conversionFailed, r, "av_hwframe_transfer_data");
        }

        if (int r = av_frame_copy_props (sw->get(), &hw); r < 0)
        {
            return fail (ErrorCode::conversionFailed, r, "av_frame_copy_props(transfer)");
        }

        return std::move (*sw);
    }

    // Final (rotated) output size with a per-request box override (RequestOptions::maximumSize).
    [[nodiscard]] Size outputSize (Size coded, AVRational sar, std::optional<Size> maxOverride) const noexcept
    {
        return fitSize (displaySize (coded, sar, applySar, rotation), maxOverride ? maxOverride : maxSize, fmt);
    }

    // Scales/converts `src` (a software frame) and applies the display transform. The SAR is
    // resolved from the container's, the frame's and the codec's declarations in that order.
    // Output and intermediate frames come from per-size buffer pools, so a steady stream of requests
    // recycles a few buffers instead of allocating a full frame per image.
    [[nodiscard]] std::expected<FramePtr, Error> convert (const AVFrame& src, AVRational containerSar,
                                                          AVRational codecSar,
                                                          std::optional<Size> maxOverride = std::nullopt)
    {
        const AVRational sar = resolveSar (containerSar, src.sample_aspect_ratio, codecSar);
        const Size coded{ src.width, src.height };
        const Size rotatedTarget = outputSize (coded, sar, maxOverride);
        const bool swap = rotation == 90 || rotation == 270;
        const Size target = swap ? rotatedTarget.transposed() : rotatedTarget;

        auto dst = pooledFrame (toAv (fmt), target.width, target.height, "output");

        if (! dst) return std::unexpected (dst.error());
        if (int r = av_frame_copy_props (dst->get(), &src); r < 0)
        {
            return fail (ErrorCode::conversionFailed, r, "av_frame_copy_props(output)");
        }

        // swscale has no fast path from semi-planar NV12 (what hardware decoders hand back) to packed
        // RGB, so a hardware frame is re-planed into YUV420P first: that copy costs far less than the
        // slow path it avoids.
        const AVFrame* in = &src;
        FramePtr replaned;

        if (needsReplaning (static_cast<AVPixelFormat> (src.format)))
        {
            auto r = replane (src);

            if (! r) return std::unexpected (std::move (r.error()));
            replaned = std::move (*r);
            in = replaned.get();
        }

        const auto srcFmt = static_cast<AVPixelFormat> (in->format);
        SwsContext* sws = scalerFor (in->width, in->height, srcFmt, target.width, target.height);

        if (sws == nullptr)
        {
            return fail (ErrorCode::conversionFailed,
                         std::string ("sws_getContext: cannot convert ")
                             + (av_get_pix_fmt_name (srcFmt) ? av_get_pix_fmt_name (srcFmt) : "?") + " to "
                             + std::string{ toString (fmt) });
        }

        applyColorspace (*in, dst->get(), sws);

        if (int r = sws_scale_frame (sws, dst->get(), in); r < 0)
        {
            return fail (ErrorCode::conversionFailed, r, "sws_scale_frame");
        }

        // Square pixels once the SAR has been resampled away; otherwise the source SAR is preserved
        // for interop consumers (transposed when the image is rotated by a quarter turn).
        if (applySar)
        {
            (*dst)->sample_aspect_ratio = AVRational{ 1, 1 };
        }
        else
        {
            (*dst)->sample_aspect_ratio = swap ? AVRational{ sar.den, sar.num } : sar;
        }

        if (stripDisplayMatrix) av_frame_remove_side_data (dst->get(), AV_FRAME_DATA_DISPLAYMATRIX);

        if (rotation != 0 || mirror)
        {
            auto rotated = pooledFrame (toAv (fmt), rotatedTarget.width, rotatedTarget.height, "transformed");

            if (! rotated) return std::unexpected (rotated.error());
            if (auto r = transformInto (**dst, *rotated->get(), fmt, rotation, mirror); ! r)
                return std::unexpected (r.error());
            return std::move (*rotated);
        }

        return std::move (*dst);
    }

private:
    // NV12/NV21 sources headed for a packed RGB or gray output take the two-step route.
    [[nodiscard]] bool needsReplaning (AVPixelFormat f) const noexcept
    {
        return (f == AV_PIX_FMT_NV12 || f == AV_PIX_FMT_NV21) && (isPackedRgb (fmt) || fmt == PixelFormat::gray8);
    }

    // NV12/NV21 -> YUV420P at the same size: a plane rearrangement (colour metadata preserved).
    [[nodiscard]] std::expected<FramePtr, Error> replane (const AVFrame& src)
    {
        auto out = pooledFrame (AV_PIX_FMT_YUV420P, src.width, src.height, "replane");

        if (! out) return std::unexpected (out.error());
        if (int r = av_frame_copy_props (out->get(), &src); r < 0)
        {
            return fail (ErrorCode::conversionFailed, r, "av_frame_copy_props(replane)");
        }

        SwsContext* ctx =
            sws_getCachedContext (swsReplane.release(), src.width, src.height, static_cast<AVPixelFormat> (src.format),
                                  src.width, src.height, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
        swsReplane.reset (ctx);

        if (! swsReplane) return fail (ErrorCode::conversionFailed, "sws_getCachedContext(replane)");
        if (int r = sws_scale_frame (swsReplane.get(), out->get(), &src); r < 0)
        {
            return fail (ErrorCode::conversionFailed, r, "sws_scale_frame(replane)");
        }

        return std::move (*out);
    }

    // A scaling context for this geometry from a small most-recently-used set, so that alternating
    // output sizes (a native-size still and a 160 px thumbnail) do not re-create one per request.
    [[nodiscard]] SwsContext* scalerFor (int sw, int sh, AVPixelFormat sfmt, int dw, int dh)
    {
        ++clock;
        std::size_t victim = 0;

        for (std::size_t i = 0; i < scalers.size(); ++i)
        {
            Scaler& s = scalers[i];

            if (s.ctx && s.sw == sw && s.sh == sh && s.sfmt == sfmt && s.dw == dw && s.dh == dh)
            {
                s.used = clock;
                return s.ctx.get();
            }

            if (! s.ctx || s.used < scalers[victim].used) victim = i;
        }

        SwsCtxPtr ctx{ sws_getContext (sw, sh, sfmt, dw, dh, toAv (fmt), swsFlags, nullptr, nullptr, nullptr) };

        if (! ctx) return nullptr;
        Scaler& s = scalers[victim];
        s.sw = sw;
        s.sh = sh;
        s.sfmt = sfmt;
        s.dw = dw;
        s.dh = dh;
        s.used = clock;
        s.ctx = std::move (ctx);
        return s.ctx.get();
    }

    // A frame whose pixel buffer comes from a pool keyed on (format, width, height): the layout is
    // the one av_frame_get_buffer() produces (64-byte aligned lines, 32-row padded height) so rows
    // may be padded exactly as before. Buffers return to the pool when the Image is destroyed.
    [[nodiscard]] std::expected<FramePtr, Error> pooledFrame (AVPixelFormat fmt, int width, int height,
                                                              const char* what)
    {
        constexpr int align = 64;
        ++clock;
        std::size_t slot = pools.size();
        std::size_t victim = 0;

        for (std::size_t i = 0; i < pools.size(); ++i)
        {
            if (pools[i].pool && pools[i].fmt == fmt && pools[i].width == width && pools[i].height == height)
            {
                slot = i;
                break;
            }

            if (! pools[i].pool || pools[i].used < pools[victim].used) victim = i;
        }

        if (slot == pools.size())
        {
            Pool& p = pools[victim];
            p.pool.reset();
            p.fmt = fmt;
            p.width = width;
            p.height = height;

            if (int r = av_image_fill_linesizes (p.linesize, fmt, FFALIGN (width, align)); r < 0)
            {
                return fail (ErrorCode::conversionFailed, r, std::string ("av_image_fill_linesizes(") + what + ")");
            }

            std::array<std::ptrdiff_t, 4> ls{};

            for (int i = 0; i < 4; ++i)
            {
                p.linesize[i] = FFALIGN (p.linesize[i], align);
                ls[static_cast<std::size_t> (i)] = p.linesize[i];
            }

            p.paddedHeight = FFALIGN (height, 32);
            std::array<std::size_t, 4> sizes{};

            if (int r = av_image_fill_plane_sizes (sizes.data(), fmt, p.paddedHeight, ls.data()); r < 0)
            {
                return fail (ErrorCode::conversionFailed, r, std::string ("av_image_fill_plane_sizes(") + what + ")");
            }

            std::size_t total = 0;

            for (std::size_t sz : sizes)
                total += sz;
            total += 4 * 16 + align; // plane padding, as av_frame_get_buffer adds
            p.pool.reset (av_buffer_pool_init (total, nullptr));

            if (! p.pool) return fail (ErrorCode::outOfMemory, std::string ("av_buffer_pool_init(") + what + ")");
            slot = victim;
        }

        Pool& pool = pools[slot];
        pool.used = clock;
        auto frame = makeFrame();

        if (! frame) return std::unexpected (frame.error());
        AVBufferRef* buf = av_buffer_pool_get (pool.pool.get());

        if (buf == nullptr) return fail (ErrorCode::outOfMemory, std::string ("av_buffer_pool_get(") + what + ")");
        (*frame)->buf[0] = buf;
        (*frame)->format = fmt;
        (*frame)->width = width;
        (*frame)->height = height;

        for (int i = 0; i < 4; ++i)
            (*frame)->linesize[i] = pool.linesize[i];

        if (int r = av_image_fill_pointers ((*frame)->data, fmt, pool.paddedHeight, buf->data, pool.linesize); r < 0)
        {
            return fail (ErrorCode::conversionFailed, r, std::string ("av_image_fill_pointers(") + what + ")");
        }

        (*frame)->extended_data = (*frame)->data;
        return std::move (*frame);
    }

    // Tells swscale the source matrix/range so 709 content is not decoded with 601 coefficients
    // and limited-range luma is expanded for RGB outputs; labels the output with what swscale was
    // told to produce.
    void applyColorspace (const AVFrame& src, AVFrame* dst, SwsContext* sws) noexcept
    {
        const auto* desc = av_pix_fmt_desc_get (static_cast<AVPixelFormat> (src.format));
        const bool srcIsRgb = desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
        const bool dstIsRgb = isPackedRgb (fmt);
        const bool dstIsGray = fmt == PixelFormat::gray8;

        int cs = src.colorspace;

        if (cs == AVCOL_SPC_UNSPECIFIED || cs == AVCOL_SPC_RGB)
        {
            cs = src.height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601;
        }

        const int* coeffs = sws_getCoefficients (cs);
        const int srcFull = src.color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        // RGB and gray outputs are full range. YUV outputs keep the source range; an RGB source has no
        // YUV range of its own, and the conventional (ffmpeg CLI) choice for RGB->YUV is limited.
        const int dstFull = (dstIsRgb || dstIsGray) ? 1 : (srcIsRgb ? 0 : srcFull);
        // Always set: even for RGB sources dstRange decides whether swscale writes limited or
        // full-range YUV, and the label below must match. A negative return means "not applicable"
        // (same-format copy).
        (void)sws_setColorspaceDetails (sws, coeffs, srcFull, coeffs, dstFull, 0, 1 << 16, 1 << 16);
        dst->colorspace = static_cast<AVColorSpace> (dstIsRgb ? AVCOL_SPC_RGB : cs);
        dst->color_range = dstFull ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    }

    PixelFormat fmt;
    int swsFlags;
    std::optional<Size> maxSize;
    bool applySar;
    int rotation;
    bool mirror;
    bool stripDisplayMatrix;
    struct Scaler
    {
        int sw{ 0 }, sh{ 0 };
        AVPixelFormat sfmt{ AV_PIX_FMT_NONE };
        int dw{ 0 }, dh{ 0 };
        std::uint64_t used{ 0 };
        SwsCtxPtr ctx;
    };

    struct Pool
    {
        AVPixelFormat fmt{ AV_PIX_FMT_NONE };
        int width{ 0 }, height{ 0 };
        int linesize[4]{};
        int paddedHeight{ 0 };
        std::uint64_t used{ 0 };
        avPtr<AVBufferPool, av_buffer_pool_uninit> pool;
    };

    std::uint64_t clock{ 0 };      // least-recently-used stamps
    std::array<Scaler, 4> scalers; // one scaling context per output geometry seen recently
    std::array<Pool, 4> pools;     // one buffer pool per output geometry seen recently
    SwsCtxPtr swsReplane;
};

// Downloads a hardware frame into a new software frame (props copied), allocating the destination.
[[nodiscard]] inline std::expected<FramePtr, Error> transferToSoftwareUnpooled (const AVFrame& hw)
{
    auto sw = makeFrame();

    if (! sw) return std::unexpected (sw.error());
    if (int r = decoderHooks().hwframeTransferData (sw->get(), &hw, 0); r < 0)
    {
        return fail (ErrorCode::conversionFailed, r, "av_hwframe_transfer_data");
    }

    if (int r = av_frame_copy_props (sw->get(), &hw); r < 0)
    {
        return fail (ErrorCode::conversionFailed, r, "av_frame_copy_props(transfer)");
    }

    return std::move (*sw);
}

} // namespace stills::detail

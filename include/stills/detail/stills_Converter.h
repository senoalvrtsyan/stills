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
[[nodiscard]] inline Size computeDisplaySize (Size coded, AVRational sar, bool applySampleAspectRatio,
                                              int rotation) noexcept
{
    Size display = coded;

    const bool hasNonSquarePixels = sar.num > 0 && sar.den > 0 && sar.num != sar.den;

    if (applySampleAspectRatio && hasNonSquarePixels)
    {
        const double scaledWidth = static_cast<double> (coded.width) * sar.num / sar.den;
        display.width = std::max (1, static_cast<int> (std::lround (scaledWidth)));
    }

    if (rotation == 90 || rotation == 270) display = display.transposed();
    return display;
}

// Fits `display` into `max` (either dimension 0/absent = unconstrained) preserving aspect ratio,
// never upscaling, and rounding down to even for chroma-subsampled formats (minimum 2x2; boxes
// smaller than that are rejected by Options::validate).
[[nodiscard]] inline Size fitWithin (Size display, std::optional<Size> maximumBox, PixelFormat format) noexcept
{
    double scale = 1.0;

    if (maximumBox)
    {
        if (maximumBox->width > 0) scale = std::min (scale, static_cast<double> (maximumBox->width) / display.width);
        if (maximumBox->height > 0) scale = std::min (scale, static_cast<double> (maximumBox->height) / display.height);
    }

    Size out{ std::max (1, static_cast<int> (std::lround (display.width * scale))),
              std::max (1, static_cast<int> (std::lround (display.height * scale))) };

    if (hasChromaSubsampling (format))
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
    const auto isPositive = [] (AVRational ratio) { return ratio.num > 0 && ratio.den > 0; };

    if (isPositive (container)) return container;
    if (isPositive (frame)) return frame;
    if (isPositive (codec)) return codec;
    return AVRational{ 1, 1 };
}

[[nodiscard]] inline std::expected<FramePtr, Error> transferToSoftwareUnpooled (const AVFrame& hardwareFrame);

// Converts decoded software frames to the configured output format/size.
class Converter
{
public:
    // `stripDisplayMatrix`: the display transform has been applied, so the output frame must not
    // carry the AV_FRAME_DATA_DISPLAYMATRIX side data any more (an interop consumer honouring it
    // would rotate a second time).
    Converter (PixelFormat outputFormat, Scaler scalerKind, std::optional<Size> maximumBox, bool resampleToSquarePixels,
               int rotationDegrees, bool mirrorHorizontally, bool removeDisplayMatrix) noexcept
      : pixelFormat (outputFormat),
        // SWS_ACCURATE_RND is deliberately not set: it disables swscale's fast YUV->RGB paths for
        // at most one LSB. SWS_FULL_CHR_H_INT (full chroma interpolation for RGB outputs) is free
        // on those paths.
        swsFlags (toSwsFlags (scalerKind) | SWS_FULL_CHR_H_INT), maximumSize (maximumBox),
        applySampleAspectRatio (resampleToSquarePixels), rotation (rotationDegrees), mirrored (mirrorHorizontally),
        stripDisplayMatrix (removeDisplayMatrix)
    {
    }

    [[nodiscard]] PixelFormat getPixelFormat() const noexcept { return pixelFormat; }
    [[nodiscard]] int getRotation() const noexcept { return rotation; }
    [[nodiscard]] bool isMirrored() const noexcept { return mirrored; }

    // Final (rotated) output size for a source of this coded size and SAR.
    [[nodiscard]] Size getOutputSize (Size coded, AVRational sar) const noexcept
    {
        return fitWithin (computeDisplaySize (coded, sar, applySampleAspectRatio, rotation), maximumSize, pixelFormat);
    }

    // Downloads a hardware frame into a pooled software frame (props copied). Pooling matters: a
    // fresh allocation per transfer is large enough for glibc to mmap it, so every transfer pays for
    // page faults on top of the DMA.
    [[nodiscard]] std::expected<FramePtr, Error> download (const AVFrame& hardwareFrame)
    {
        AVPixelFormat softwareFormat = AV_PIX_FMT_NONE;

        if (hardwareFrame.hw_frames_ctx != nullptr)
            softwareFormat = reinterpret_cast<const AVHWFramesContext*> (hardwareFrame.hw_frames_ctx->data)->sw_format;

        if (softwareFormat == AV_PIX_FMT_NONE) return transferToSoftwareUnpooled (hardwareFrame);
        auto softwareFrame = pooledFrame (softwareFormat, hardwareFrame.width, hardwareFrame.height, "download");

        if (! softwareFrame) return std::unexpected (softwareFrame.error());
        // Through the hook (detail/stills_DecoderHooks.h) so the rebuild-once path a driver fault
        // drives can be tested on the shipped code. Defaults to av_hwframe_transfer_data itself.
        if (int result = decoderHooks().hwframeTransferData (softwareFrame->get(), &hardwareFrame, 0); result < 0)
        {
            return fail (ErrorCode::conversionFailed, result, "av_hwframe_transfer_data");
        }

        if (int result = av_frame_copy_props (softwareFrame->get(), &hardwareFrame); result < 0)
        {
            return fail (ErrorCode::conversionFailed, result, "av_frame_copy_props(transfer)");
        }

        return std::move (*softwareFrame);
    }

    // Final (rotated) output size with a per-request box override (RequestOptions::maximumSize).
    [[nodiscard]] Size getOutputSize (Size coded, AVRational sar, std::optional<Size> maxOverride) const noexcept
    {
        return fitWithin (computeDisplaySize (coded, sar, applySampleAspectRatio, rotation),
                          maxOverride ? maxOverride : maximumSize, pixelFormat);
    }

    // Scales/converts `src` (a software frame) and applies the display transform. The SAR is
    // resolved from the container's, the frame's and the codec's declarations in that order.
    // Output and intermediate frames come from per-size buffer pools, so a steady stream of requests
    // recycles a few buffers instead of allocating a full frame per image.
    [[nodiscard]] std::expected<FramePtr, Error> convert (const AVFrame& source, AVRational containerSar,
                                                          AVRational codecSar,
                                                          std::optional<Size> maxOverride = std::nullopt)
    {
        const AVRational sar = resolveSar (containerSar, source.sample_aspect_ratio, codecSar);
        const Size coded{ source.width, source.height };
        const Size rotatedTarget = getOutputSize (coded, sar, maxOverride);
        const bool swap = rotation == 90 || rotation == 270;
        const Size target = swap ? rotatedTarget.transposed() : rotatedTarget;

        auto destination = pooledFrame (toAv (pixelFormat), target.width, target.height, "output");

        if (! destination) return std::unexpected (destination.error());
        if (int result = av_frame_copy_props (destination->get(), &source); result < 0)
        {
            return fail (ErrorCode::conversionFailed, result, "av_frame_copy_props(output)");
        }

        // swscale has no fast path from semi-planar NV12 (what hardware decoders hand back) to packed
        // RGB, so a hardware frame is re-planed into YUV420P first: that copy costs far less than the
        // slow path it avoids.
        const AVFrame* input = &source;
        FramePtr replanedFrame;

        if (needsReplaning (static_cast<AVPixelFormat> (source.format)))
        {
            auto result = replane (source);

            if (! result) return std::unexpected (std::move (result.error()));
            replanedFrame = std::move (*result);
            input = replanedFrame.get();
        }

        const auto sourceFormat = static_cast<AVPixelFormat> (input->format);
        SwsContext* scalerContext = scalerFor (input->width, input->height, sourceFormat, target.width, target.height);

        if (scalerContext == nullptr)
        {
            return fail (ErrorCode::conversionFailed,
                         std::string ("sws_getContext: cannot convert ")
                             + (av_get_pix_fmt_name (sourceFormat) ? av_get_pix_fmt_name (sourceFormat) : "?") + " to "
                             + std::string{ toString (pixelFormat) });
        }

        applyColorspace (*input, destination->get(), scalerContext);

        if (int result = sws_scale_frame (scalerContext, destination->get(), input); result < 0)
        {
            return fail (ErrorCode::conversionFailed, result, "sws_scale_frame");
        }

        // Square pixels once the SAR has been resampled away; otherwise the source SAR is preserved
        // for interop consumers (transposed when the image is rotated by a quarter turn).
        if (applySampleAspectRatio)
        {
            (*destination)->sample_aspect_ratio = AVRational{ 1, 1 };
        }
        else
        {
            (*destination)->sample_aspect_ratio = swap ? AVRational{ sar.den, sar.num } : sar;
        }

        if (stripDisplayMatrix) av_frame_remove_side_data (destination->get(), AV_FRAME_DATA_DISPLAYMATRIX);

        if (rotation != 0 || mirrored)
        {
            auto rotated = pooledFrame (toAv (pixelFormat), rotatedTarget.width, rotatedTarget.height, "transformed");

            if (! rotated) return std::unexpected (rotated.error());
            if (auto result = transformInto (**destination, *rotated->get(), pixelFormat, rotation, mirrored); ! result)
                return std::unexpected (result.error());
            return std::move (*rotated);
        }

        return std::move (*destination);
    }

private:
    // One cached scaling context and the geometry it was built for.
    struct ScalerEntry
    {
        int sourceWidth{ 0 };
        int sourceHeight{ 0 };
        AVPixelFormat sourceFormat{ AV_PIX_FMT_NONE };
        int destinationWidth{ 0 };
        int destinationHeight{ 0 };
        std::uint64_t used{ 0 };
        SwsCtxPtr context;
    };

    // One buffer pool and the frame geometry its buffers are laid out for.
    struct Pool
    {
        AVPixelFormat format{ AV_PIX_FMT_NONE };
        int width{ 0 };
        int height{ 0 };
        int linesize[4]{};
        int paddedHeight{ 0 };
        std::uint64_t used{ 0 };
        avPtr<AVBufferPool, av_buffer_pool_uninit> pool;
    };

    // NV12/NV21 sources headed for a packed RGB or gray output take the two-step route.
    [[nodiscard]] bool needsReplaning (AVPixelFormat format) const noexcept
    {
        return (format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_NV21)
               && (isPackedRgb (pixelFormat) || pixelFormat == PixelFormat::gray8);
    }

    // NV12/NV21 -> YUV420P at the same size: a plane rearrangement (colour metadata preserved).
    [[nodiscard]] std::expected<FramePtr, Error> replane (const AVFrame& source)
    {
        auto out = pooledFrame (AV_PIX_FMT_YUV420P, source.width, source.height, "replane");

        if (! out) return std::unexpected (out.error());
        if (int result = av_frame_copy_props (out->get(), &source); result < 0)
        {
            return fail (ErrorCode::conversionFailed, result, "av_frame_copy_props(replane)");
        }

        SwsContext* context = sws_getCachedContext (
            swsReplane.release(), source.width, source.height, static_cast<AVPixelFormat> (source.format), source.width,
            source.height, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
        swsReplane.reset (context);

        if (! swsReplane) return fail (ErrorCode::conversionFailed, "sws_getCachedContext(replane)");
        if (int result = sws_scale_frame (swsReplane.get(), out->get(), &source); result < 0)
        {
            return fail (ErrorCode::conversionFailed, result, "sws_scale_frame(replane)");
        }

        return std::move (*out);
    }

    // A scaling context for this geometry from a small most-recently-used set, so that alternating
    // output sizes (a native-size still and a 160 px thumbnail) do not re-create one per request.
    [[nodiscard]] SwsContext* scalerFor (int sourceWidth, int sourceHeight, AVPixelFormat sourceFormat,
                                         int destinationWidth, int destinationHeight)
    {
        ++clock;
        std::size_t leastRecentlyUsed = 0;

        for (std::size_t index = 0; index < scalers.size(); ++index)
        {
            ScalerEntry& entry = scalers[index];

            if (entry.context && entry.sourceWidth == sourceWidth && entry.sourceHeight == sourceHeight
                && entry.sourceFormat == sourceFormat && entry.destinationWidth == destinationWidth
                && entry.destinationHeight == destinationHeight)
            {
                entry.used = clock;
                return entry.context.get();
            }

            if (! entry.context || entry.used < scalers[leastRecentlyUsed].used) leastRecentlyUsed = index;
        }

        SwsCtxPtr context{ sws_getContext (sourceWidth, sourceHeight, sourceFormat, destinationWidth, destinationHeight,
                                           toAv (pixelFormat), swsFlags, nullptr, nullptr, nullptr) };

        if (! context) return nullptr;
        ScalerEntry& entry = scalers[leastRecentlyUsed];
        entry.sourceWidth = sourceWidth;
        entry.sourceHeight = sourceHeight;
        entry.sourceFormat = sourceFormat;
        entry.destinationWidth = destinationWidth;
        entry.destinationHeight = destinationHeight;
        entry.used = clock;
        entry.context = std::move (context);
        return entry.context.get();
    }

    // A frame whose pixel buffer comes from a pool keyed on (format, width, height): the layout is
    // the one av_frame_get_buffer() produces (64-byte aligned lines, 32-row padded height) so rows
    // may be padded exactly as before. Buffers return to the pool when the Image is destroyed.
    [[nodiscard]] std::expected<FramePtr, Error> pooledFrame (AVPixelFormat format, int width, int height,
                                                              const char* purpose)
    {
        constexpr int align = 64;
        ++clock;
        std::size_t slot = pools.size();
        std::size_t leastRecentlyUsed = 0;

        for (std::size_t index = 0; index < pools.size(); ++index)
        {
            if (pools[index].pool && pools[index].format == format && pools[index].width == width
                && pools[index].height == height)
            {
                slot = index;
                break;
            }

            if (! pools[index].pool || pools[index].used < pools[leastRecentlyUsed].used) leastRecentlyUsed = index;
        }

        if (slot == pools.size())
        {
            Pool& pool = pools[leastRecentlyUsed];
            pool.pool.reset();
            pool.format = format;
            pool.width = width;
            pool.height = height;

            if (int result = av_image_fill_linesizes (pool.linesize, format, FFALIGN (width, align)); result < 0)
            {
                return fail (ErrorCode::conversionFailed, result,
                             std::string ("av_image_fill_linesizes(") + purpose + ")");
            }

            std::array<std::ptrdiff_t, 4> lineSizes{};

            for (int index = 0; index < 4; ++index)
            {
                pool.linesize[index] = FFALIGN (pool.linesize[index], align);
                lineSizes[static_cast<std::size_t> (index)] = pool.linesize[index];
            }

            pool.paddedHeight = FFALIGN (height, 32);
            std::array<std::size_t, 4> sizes{};

            if (int result = av_image_fill_plane_sizes (sizes.data(), format, pool.paddedHeight, lineSizes.data());
                result < 0)
            {
                return fail (ErrorCode::conversionFailed, result,
                             std::string ("av_image_fill_plane_sizes(") + purpose + ")");
            }

            std::size_t total = 0;

            for (std::size_t planeSize : sizes)
                total += planeSize;
            total += 4 * 16 + align; // plane padding, as av_frame_get_buffer adds
            pool.pool.reset (av_buffer_pool_init (total, nullptr));

            if (! pool.pool) return fail (ErrorCode::outOfMemory, std::string ("av_buffer_pool_init(") + purpose + ")");
            slot = leastRecentlyUsed;
        }

        Pool& pool = pools[slot];
        pool.used = clock;
        auto frame = makeFrame();

        if (! frame) return std::unexpected (frame.error());
        AVBufferRef* buffer = av_buffer_pool_get (pool.pool.get());

        if (buffer == nullptr)
            return fail (ErrorCode::outOfMemory, std::string ("av_buffer_pool_get(") + purpose + ")");
        (*frame)->buf[0] = buffer;
        (*frame)->format = format;
        (*frame)->width = width;
        (*frame)->height = height;

        for (int index = 0; index < 4; ++index)
            (*frame)->linesize[index] = pool.linesize[index];

        if (int result =
                av_image_fill_pointers ((*frame)->data, format, pool.paddedHeight, buffer->data, pool.linesize);
            result < 0)
        {
            return fail (ErrorCode::conversionFailed, result, std::string ("av_image_fill_pointers(") + purpose + ")");
        }

        (*frame)->extended_data = (*frame)->data;
        return std::move (*frame);
    }

    // Tells swscale the source matrix/range so 709 content is not decoded with 601 coefficients
    // and limited-range luma is expanded for RGB outputs; labels the output with what swscale was
    // told to produce.
    void applyColorspace (const AVFrame& source, AVFrame* destination, SwsContext* scalerContext) noexcept
    {
        const auto* descriptor = av_pix_fmt_desc_get (static_cast<AVPixelFormat> (source.format));
        const bool sourceIsRgb = descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
        const bool destinationIsRgb = isPackedRgb (pixelFormat);
        const bool destinationIsGray = pixelFormat == PixelFormat::gray8;

        int colorSpace = source.colorspace;

        if (colorSpace == AVCOL_SPC_UNSPECIFIED || colorSpace == AVCOL_SPC_RGB)
        {
            colorSpace = source.height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601;
        }

        const int* coefficients = sws_getCoefficients (colorSpace);
        const int sourceFullRange = source.color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        // RGB and gray outputs are full range. YUV outputs keep the source range; an RGB source has no
        // YUV range of its own, and the conventional (ffmpeg CLI) choice for RGB->YUV is limited.
        const int destinationFullRange =
            (destinationIsRgb || destinationIsGray) ? 1 : (sourceIsRgb ? 0 : sourceFullRange);
        // Always set: even for RGB sources dstRange decides whether swscale writes limited or
        // full-range YUV, and the label below must match. A negative return means "not applicable"
        // (same-format copy).
        (void)sws_setColorspaceDetails (scalerContext, coefficients, sourceFullRange, coefficients,
                                        destinationFullRange, 0, 1 << 16, 1 << 16);
        destination->colorspace = static_cast<AVColorSpace> (destinationIsRgb ? AVCOL_SPC_RGB : colorSpace);
        destination->color_range = destinationFullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    }

    PixelFormat pixelFormat;
    int swsFlags;
    std::optional<Size> maximumSize;
    bool applySampleAspectRatio;
    int rotation;
    bool mirrored;
    bool stripDisplayMatrix;
    std::uint64_t clock{ 0 };           // least-recently-used stamps
    std::array<ScalerEntry, 4> scalers; // one scaling context per output geometry seen recently
    std::array<Pool, 4> pools;          // one buffer pool per output geometry seen recently
    SwsCtxPtr swsReplane;
};

// Downloads a hardware frame into a new software frame (props copied), allocating the destination.
[[nodiscard]] inline std::expected<FramePtr, Error> transferToSoftwareUnpooled (const AVFrame& hardwareFrame)
{
    auto softwareFrame = makeFrame();

    if (! softwareFrame) return std::unexpected (softwareFrame.error());
    if (int result = decoderHooks().hwframeTransferData (softwareFrame->get(), &hardwareFrame, 0); result < 0)
    {
        return fail (ErrorCode::conversionFailed, result, "av_hwframe_transfer_data");
    }

    if (int result = av_frame_copy_props (softwareFrame->get(), &hardwareFrame); result < 0)
    {
        return fail (ErrorCode::conversionFailed, result, "av_frame_copy_props(transfer)");
    }

    return std::move (*softwareFrame);
}

} // namespace stills::detail

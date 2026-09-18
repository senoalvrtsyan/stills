#pragma once
// stills/detail/stills_AssetInfoBuilder.h — assembles the AssetInfo a generator reports, from what
// the source, the decoder and the probe frame established at open.

#include <string>

#include "stills/detail/stills_Converter.h"
#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_MediaSource.h"
#include "stills/detail/stills_VideoDecoder.h"
#include "stills/stills_AssetInfo.h"
#include "stills/stills_Options.h"
#include "stills/stills_Time.h"

namespace stills::detail
{

// `probeFrame` is the first decoded frame, or nullptr: it settles the coded size and the pixel
// format where the container declared neither, and takes part in the sample-aspect-ratio
// resolution the way av_guess_sample_aspect_ratio does.
[[nodiscard]] inline AssetInfo makeAssetInfo (const MediaSource& source, const VideoDecoder& decoder,
                                              const Converter& converter, const AVFrame* probeFrame,
                                              const Options& options)
{
    const StreamInfo& stream = source.getStreamInfo();
    const AVCodecParameters& parameters = *source.getStream()->codecpar;
    const auto nameOrEmpty = [] (const char* name) { return name != nullptr ? std::string{ name } : std::string{}; };

    AssetInfo info;
    info.containerName = source.getContainerName();
    info.codecName = decoder.getCodecName();
    info.sourcePixelFormat = nameOrEmpty (av_get_pix_fmt_name (static_cast<AVPixelFormat> (parameters.format)));

    if (info.sourcePixelFormat.empty() && probeFrame != nullptr)
    {
        info.sourcePixelFormat = nameOrEmpty (av_get_pix_fmt_name (static_cast<AVPixelFormat> (probeFrame->format)));
    }

    info.videoStreamIndex = stream.index;
    info.timeBase = fromAv (stream.timeBase);
    info.averageFrameRate = stream.avgFrameRate.num > 0 ? fromAv (stream.avgFrameRate) : Rational{ 0, 1 };

    if (stream.durationPts) info.duration = Time::fromTimestamp (*stream.durationPts, info.timeBase);
    if (source.getStream()->nb_frames > 0) info.frameCount = source.getStream()->nb_frames;
    Size codedSize{ parameters.width, parameters.height };

    if (probeFrame != nullptr && probeFrame->width > 0) codedSize = Size{ probeFrame->width, probeFrame->height };
    const AVRational probeSar = probeFrame != nullptr ? probeFrame->sample_aspect_ratio : AVRational{ 0, 1 };
    const AVRational sampleAspectRatio = resolveSar (stream.containerSar, probeSar, stream.codecSar);
    const int rotation = options.applyPreferredTrackTransform ? stream.transform.rotation : 0;
    info.codedSize = codedSize;
    info.displaySize = computeDisplaySize (codedSize, sampleAspectRatio, options.applySampleAspectRatio, rotation);
    info.outputSize = converter.getOutputSize (codedSize, sampleAspectRatio);
    info.rotationDegrees = stream.transform.rotation;
    info.mirrored = stream.transform.mirrored;
    info.sampleAspectRatio = fromAv (sampleAspectRatio);
    info.seekable = stream.seekable;
    info.timestampsSynthesized = stream.synthesizeTimestamps;

    if (parameters.color_trc != AVCOL_TRC_UNSPECIFIED)
    {
        info.colorTransfer = nameOrEmpty (av_color_transfer_name (parameters.color_trc));
    }

    if (parameters.color_primaries != AVCOL_PRI_UNSPECIFIED)
    {
        info.colorPrimaries = nameOrEmpty (av_color_primaries_name (parameters.color_primaries));
    }

    if (parameters.color_space != AVCOL_SPC_UNSPECIFIED)
    {
        info.colorSpace = nameOrEmpty (av_color_space_name (parameters.color_space));
    }

    return info;
}

} // namespace stills::detail

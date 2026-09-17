#pragma once
// stills/stills_AssetInfo.h — read-only facts about an opened asset. No FFmpeg dependency.

#include <cstdint>
#include <optional>
#include <string>

#include "stills/stills_Geometry.h"
#include "stills/stills_Options.h"
#include "stills/stills_Time.h"

namespace stills
{

/// Immutable description of the opened asset, captured at open().
struct AssetInfo
{
    std::string containerName = {};     ///< AVInputFormat::name, e.g. "mov,mp4,m4a,3gp,3g2,mj2"
    std::string codecName = {};         ///< e.g. "h264"
    std::string sourcePixelFormat = {}; ///< e.g. "yuv420p"
    int videoStreamIndex{ -1 };
    Rational timeBase = {};                                ///< the stream's time base (getActualTime() is exact in it)
    Rational averageFrameRate = {};                        ///< {0,1} when unknown
    std::optional<Time> duration = std::nullopt;           ///< stream duration, else container duration, else nullopt
    std::optional<std::int64_t> frameCount = std::nullopt; ///< container-declared frame count when known
    Size codedSize = {};                                   ///< as stored in the stream
    Size displaySize = {};                                 ///< after sample-aspect-ratio and rotation
    Size outputSize = {};                                  ///< displaySize fitted into Options::maximumSize
    /// Sample aspect ratio in effect (container declaration, else bitstream), 1:1 when square.
    Rational sampleAspectRatio = { 1, 1 };
    int rotationDegrees{ 0 }; ///< 0/90/180/270 *clockwise* rotation applied for upright display (a
                              ///< 90° CCW display matrix reports 270)
    /// The display matrix also mirrors the picture horizontally (after the rotation). Applied
    /// together with the rotation when Options::applyPreferredTrackTransform is set.
    bool mirrored{ false };
    /// The container can be positioned by timestamp. False for pipes and raw elementary streams,
    /// whose backward requests re-open the input or fail (see README "Known limitations").
    bool seekable{ true };
    /// The container carries no timestamps; frames are stamped from the codec frame rate in display
    /// order (raw H.264/HEVC elementary streams). getActualTime() is then a reconstruction.
    bool timestampsSynthesized{ false };
    /// Colour metadata of the source as declared (libav names: "bt709", "smpte2084" (PQ),
    /// "arib-std-b67" (HLG), "bt2020", ...; empty when unspecified). The library maps the matrix and
    /// range only; a compositor must tone-map PQ/HLG itself (README "Colour").
    std::string colorTransfer = {};
    std::string colorPrimaries = {};
    std::string colorSpace = {};

    /// [0, duration) when the duration is known.
    [[nodiscard]] std::optional<TimeRange> getTimeRange() const noexcept
    {
        if (! duration) return std::nullopt;
        return TimeRange{ Time::zero(), *duration };
    }
};

/// Which decode path ended up active after open().
struct ActiveDecoder
{
    std::string decoderName = {}; ///< AVCodec::name
    bool hardware{ false };
    /// The device family, when it is one this library names; nullopt for a family FFmpeg knows but
    /// this enum does not (see deviceTypeName).
    std::optional<HardwareDeviceType> deviceType = std::nullopt;
    std::string deviceTypeName = {}; ///< av_hwdevice_get_type_name, set when hardware == true
    std::string device = {};         ///< device string actually used
    std::string fallbackReason = {}; ///< why hardware was not used (preferHardware/automatic only)
    int decoderThreads{ 0 };         ///< frame threads the decoder was opened with (1 for hardware)
};

} // namespace stills

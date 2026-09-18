#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stills/detail/stills_DecoderHooks.h>
#include <stills/stills_Interop.h>
#include <string>
#include <utility>
#include <vector>

#include "support/stills_TestCommon.h"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::HardwareDeviceType;
using stills::HardwarePolicy;
using stills::PixelFormat;
using stills::Time;

namespace
{
stills::Options hwOptions (HardwarePolicy policy, PixelFormat fmt = PixelFormat::rgba)
{
    stills::Options o;
    o.hardware.policy = policy;
    o.pixelFormat = fmt;
    return o;
}
} // namespace

TEST_CASE ("hw: requireHardware with an unusable device fails deterministically", "[hw]")
{
    stills::Options o = hwOptions (HardwarePolicy::requireHardware);
    o.hardware.deviceType = HardwareDeviceType::vaapi;
    o.hardware.device = "/dev/dri/does-not-exist";
    auto g = AssetImageGenerator::open (fixture ("counter.mp4").string(), o);
    REQUIRE_ERROR (g, ErrorCode::hardwareUnavailable);
    CHECK_FALSE (g.error().message.empty());
}

TEST_CASE ("hw: preferHardware with an unusable device falls back to software", "[hw]")
{
    stills::Options o = hwOptions (HardwarePolicy::preferHardware);
    o.hardware.deviceType = HardwareDeviceType::vaapi;
    o.hardware.device = "/dev/dri/does-not-exist";
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mp4").string(), o));
    CHECK_FALSE (g.getActiveDecoder().hardware);
    CHECK_FALSE (g.getActiveDecoder().fallbackReason.empty());

    for (int n : { 0, 29, 30, 77 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        CHECK (frameIndexOf (img) == n);
    }
}

TEST_CASE ("hw: automatic selection is correct whether or not hardware is present", "[hw][auto]")
{
    auto hw = REQUIRE_OK (
        AssetImageGenerator::open (fixture ("counter.mp4").string(), hwOptions (HardwarePolicy::preferHardware)));
    auto sw = openCounter (swOptions (PixelFormat::rgba));
    WARN ("active decoder: " << (hw.getActiveDecoder().hardware
                                     ? "hardware (" + std::string{ toString (*hw.getActiveDecoder().deviceType) } + ")"
                                     : "software: " + hw.getActiveDecoder().fallbackReason));

    for (int n : { 0, 1, 29, 30, 31, 60, 119, 45 })
    {
        auto a = REQUIRE_OK (hw.imageAt (Time{ n, 30 }));
        auto b = REQUIRE_OK (sw.imageAt (Time{ n, 30 }));
        CHECK (frameIndexOf (a) == n);
        CHECK (a.getActualTime() == Time{ n, 30 });
        CHECK (a.isKeyframe() == (n % 30 == 0));
        REQUIRE (a.getSize() == b.getSize());
        const auto pa = REQUIRE_OK (a.toPackedBytes());
        const auto pb = REQUIRE_OK (b.toPackedBytes());
        REQUIRE (pa.size() == pb.size());
        int maxDiff = 0;

        for (std::size_t i = 0; i < pa.size(); ++i)
        {
            maxDiff = std::max (maxDiff, std::abs (std::to_integer<int> (pa[i]) - std::to_integer<int> (pb[i])));
        }

        CHECK (maxDiff <= 2);
    }

    if (hw.getActiveDecoder().hardware)
    {
        // Hardware frames come back through av_hwframe_transfer_data; a scaled output exercises it too.
        stills::Options o = hwOptions (HardwarePolicy::preferHardware, PixelFormat::nv12);
        o.maximumSize = stills::Size{ 32, 0 };
        auto scaled = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mp4").string(), o));
        auto img = REQUIRE_OK (scaled.imageAt (Time{ 88, 30 }));
        CHECK (img.getSize() == stills::Size{ 32, 24 });
        CHECK (frameIndexOf (img) == 88);
    }
}

TEST_CASE ("hw: requireHardware with automatic selection", "[hw][auto]")
{
    const auto available = stills::interop::getAvailableDeviceTypes();
    auto g = AssetImageGenerator::open (fixture ("counter.mp4").string(), hwOptions (HardwarePolicy::requireHardware));

    if (! g)
    {
        INFO ("error: " << g.error());
        CHECK (g.error().code == ErrorCode::hardwareUnavailable);
        SKIP ("no usable hardware decoder on this machine (" << available.size() << " device types probed)");
    }

    CHECK (g->getActiveDecoder().hardware);
    REQUIRE (g->getActiveDecoder().deviceType.has_value());
    auto img = REQUIRE_OK (g->imageAt (Time{ 59, 30 }));
    CHECK (frameIndexOf (img) == 59);
}

#if __has_include(<unistd.h>)
#include <fcntl.h>
#include <unistd.h>
TEST_CASE ("hw: non-rewindable input never probes hardware", "[hw]")
{
    // The reason string under test is the one the pipeline writes once it has hardware candidates
    // to refuse. An FFmpeg built with no hardware device types produces no candidates at all, and
    // reports that instead, so there is nothing for this test to check on such a build.
    if (av_hwdevice_iterate_types (AV_HWDEVICE_TYPE_NONE) == AV_HWDEVICE_TYPE_NONE)
        SKIP ("this FFmpeg build has no hardware device types");

    SECTION ("preferHardware falls back to software with the reason")
    {
        const int fd = ::open (fixture ("counter.mp4").c_str(), O_RDONLY);
        REQUIRE (fd >= 0);
        auto g = REQUIRE_OK (
            AssetImageGenerator::open ("pipe:" + std::to_string (fd), hwOptions (HardwarePolicy::preferHardware)));
        CHECK_FALSE (g.getActiveDecoder().hardware);
        CHECK (g.getActiveDecoder().fallbackReason.find ("non-rewindable") != std::string::npos);
        auto img = REQUIRE_OK (g.imageAt (Time{ 9, 30 }));
        CHECK (frameIndexOf (img) == 9);
        ::close (fd);
    }

    SECTION ("requireHardware fails with hardwareUnavailable")
    {
        const int fd = ::open (fixture ("counter.mp4").c_str(), O_RDONLY);
        REQUIRE (fd >= 0);
        auto g = AssetImageGenerator::open ("pipe:" + std::to_string (fd), hwOptions (HardwarePolicy::requireHardware));
        REQUIRE_ERROR (g, ErrorCode::hardwareUnavailable);
        CHECK (g.error().message.find ("non-rewindable") != std::string::npos);
        ::close (fd);
    }
}
#endif

TEST_CASE ("hw: the automatic policy uses software for a small H.264 clip", "[hw]")
{
    auto g = REQUIRE_OK (
        AssetImageGenerator::open (fixture ("counter.mp4").string(), hwOptions (HardwarePolicy::automatic)));
    CHECK_FALSE (g.getActiveDecoder().hardware);
    CHECK (g.getActiveDecoder().fallbackReason.find ("automatic") != std::string::npos);
    auto img = REQUIRE_OK (g.imageAt (Time{ 44, 30 }));
    CHECK (frameIndexOf (img) == 44);
}

TEST_CASE ("hw: explicit device type that the build lacks", "[hw]")
{
    stills::Options o = hwOptions (HardwarePolicy::preferHardware);
    o.hardware.deviceType = HardwareDeviceType::mediacodec; // Android only
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mp4").string(), o));
    CHECK_FALSE (g.getActiveDecoder().hardware);
    auto img = REQUIRE_OK (g.imageAt (Time{ 12, 30 }));
    CHECK (frameIndexOf (img) == 12);
}

// A stream that switches to a profile the hardware decoder rejects mid-way: the hardware decoder is
// rebuilt once, then the generator falls back to software and keeps serving exact frames from both
// segments.
TEST_CASE ("hw: a mid-stream profile switch falls back to software once, exactly", "[hw][fallback]")
{
    // Software first: the h264 decoder re-initialises on the SPS change and both segments decode.
    {
        auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_profile_switch.ts").string(), swOptions()));

        for (int n : { 50, 150, 239, 10, 200 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
            INFO ("software frame " << n);
            CHECK (frameIndexOf (img) == n);
        }
    }

    stills::Options o = hwOptions (HardwarePolicy::preferHardware);
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_profile_switch.ts").string(), o));

    if (! g.getActiveDecoder().hardware)
        SKIP ("no hardware decoder for the first segment on this machine (" << g.getActiveDecoder().fallbackReason
                                                                            << ")");
    const std::string device = g.getActiveDecoder().deviceTypeName;
    auto a = REQUIRE_OK (g.imageAt (Time{ 50, 30 }));
    CHECK (frameIndexOf (a) == 50);
    CHECK (g.getActiveDecoder().hardware);
    // Into the 4:4:4 segment: the hardware decoder declines it. VAAPI accepts High only.
    auto b = REQUIRE_OK (g.imageAt (Time{ 150, 30 }));
    CHECK (frameIndexOf (b) == 150);
    const auto after = g.getActiveDecoder();
    WARN ("device " << device << ": after the switch hardware=" << after.hardware << " reason='" << after.fallbackReason
                    << "'");

    if (! after.hardware) CHECK_FALSE (after.fallbackReason.empty()); // rebuilt once, then software
    for (int n : { 239, 10, 200, 119, 120 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        INFO ("frame " << n << " after the switch");
        CHECK (frameIndexOf (img) == n);
    }

    // requireHardware: the same fault is an error, not a silent software frame.
    stills::Options strict = hwOptions (HardwarePolicy::requireHardware);
    auto h = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_profile_switch.ts").string(), strict));
    auto r = h.imageAt (Time{ 150, 30 });

    if (! r)
        CHECK (r.error().code == ErrorCode::hardwareUnavailable);
    else
        CHECK (frameIndexOf (*r) == 150); // a decoder that handles 4:4:4 (CUDA does) simply succeeds
}

// ---- the mid-stream hardware fault: rebuild once, then fall back -----------------------------
// A driver that loses a session, or a surface download that fails with EIO, is usually transient,
// so the library re-creates the same hardware decoder once before giving up on it and rebuilding
// in software. No driver here fails on demand, so the fault is injected at the one libav call that
// reports it — av_hwframe_transfer_data, replaced through detail::DecoderHooks (see
// include/stills/detail/stills_DecoderHooks.h for why that call and not a per-packet one). Everything else in
// the path is the shipped code.
namespace
{

int gTransfersToFail = 0; ///< how many of the next transfers the hook fails
int gTransfersFailed = 0;

int failingTransfer (AVFrame* dst, const AVFrame* src, int flags)
{
    if (gTransfersToFail != 0)
    {
        if (gTransfersToFail > 0) --gTransfersToFail;
        ++gTransfersFailed;
        return AVERROR (EIO); // what a lost VAAPI/NVDEC surface actually reports
    }

    return av_hwframe_transfer_data (dst, src, flags);
}

/// A generator on a real hardware decoder, or nullopt when this machine has none.
std::optional<AssetImageGenerator> openHw (HardwarePolicy policy = HardwarePolicy::preferHardware)
{
    auto g = AssetImageGenerator::open (fixture ("counter.mp4").string(), hwOptions (policy));

    if (! g || ! g->getActiveDecoder().hardware) return std::nullopt;
    return std::move (*g);
}

} // namespace

TEST_CASE ("hw: a transient download failure rebuilds the decoder once and still returns the frame", "[hw][fallback]")
{
    if (! openHw()) SKIP ("no hardware decoder on this machine");
    gTransfersToFail = 0;
    gTransfersFailed = 0;

    SECTION ("one failure: rebuilt once, still hardware, right frame")
    {
        auto g = openHw();
        REQUIRE (g.has_value());
        {
            stills::detail::ScopedHook hook{ { &failingTransfer } };
            gTransfersToFail = 1; // the next transfer only
            auto img = g->imageAt (Time{ 47, 30 });
            INFO ("error: " << (img ? std::string{ "none" } : toString (img.error())));
            REQUIRE (img.has_value());
            CHECK (frameIndexOf (*img) == 47);
        }

        CHECK (gTransfersFailed == 1);          // one download failed; the retry after the rebuild did not
        CHECK (g->getActiveDecoder().hardware); // the fault was transient: hardware is kept
        auto again = REQUIRE_OK (g->imageAt (Time{ 11, 30 }));
        CHECK (frameIndexOf (again) == 11);
        CHECK (gTransfersFailed == 1); // no further fault, so no further rebuild
    }

    SECTION ("every transfer fails: rebuilt once, then software, and the frame still arrives")
    {
        auto g = openHw (HardwarePolicy::preferHardware);
        REQUIRE (g.has_value());
        stills::Image img = [&]
        {
            stills::detail::ScopedHook hook{ { &failingTransfer } };
            gTransfersToFail = -1; // forever
            auto r = g->imageAt (Time{ 52, 30 });
            gTransfersToFail = 0;
            INFO ("error: " << (r ? std::string{ "none" } : toString (r.error())));
            REQUIRE (r.has_value());
            return std::move (*r);
        }();

        CHECK (frameIndexOf (img) == 52); // the consumer gets their frame, from software
        // Two downloads were attempted and failed: the original and the one after the single hardware
        // rebuild. A third would mean the decoder was rebuilt more than once before falling back.
        CHECK (gTransfersFailed == 2);
        CHECK_FALSE (g->getActiveDecoder().hardware);
        INFO ("fallbackReason: " << g->getActiveDecoder().fallbackReason);
        CHECK (g->getActiveDecoder().fallbackReason.find ("hardware decoder failed during decoding")
               != std::string::npos);
        // The generator stays usable on the software decoder.
        auto later = REQUIRE_OK (g->imageAt (Time{ 7, 30 }));
        CHECK (frameIndexOf (later) == 7);
    }

    SECTION ("requireHardware: no software fallback, a clear error, and the generator survives")
    {
        auto probe =
            AssetImageGenerator::open (fixture ("counter.mp4").string(), hwOptions (HardwarePolicy::requireHardware));

        if (! probe) SKIP ("requireHardware cannot be satisfied here");
        {
            stills::detail::ScopedHook hook{ { &failingTransfer } };
            gTransfersToFail = -1;
            auto r = probe->imageAt (Time{ 30, 30 });
            gTransfersToFail = 0;
            REQUIRE_FALSE (r.has_value());
            INFO ("error: " << r.error());
            CHECK (r.error().code == ErrorCode::hardwareUnavailable);
        }

        CHECK (gTransfersFailed == 2);                          // original + one after the single rebuild
        CHECK (probe->getActiveDecoder().hardware);             // never silently downgraded
        auto ok = REQUIRE_OK (probe->imageAt (Time{ 30, 30 })); // works again once the driver does
        CHECK (frameIndexOf (ok) == 30);
    }

    gTransfersToFail = 0;
}

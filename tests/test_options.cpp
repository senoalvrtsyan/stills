// Options::validate(), interop preconditions, Time conveniences.
#include <chrono>
#include <limits>
#include <stills/stills_Interop.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "support/stills_TestCommon.h"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::Options;
using stills::Time;

TEST_CASE ("options: validate() rejects every documented misuse", "[open][options]")
{
    const auto rejects = [] (Options o, const char* what)
    {
        INFO (what);
        auto v = o.validate();
        REQUIRE_FALSE (v.has_value());
        CHECK (v.error().code == ErrorCode::invalidArgument);
        CHECK_FALSE (v.error().message.empty());
        REQUIRE_ERROR (AssetImageGenerator::open (fixture ("counter.mp4").string(), o), ErrorCode::invalidArgument);
    };

    Options o = swOptions();
    REQUIRE_OK (o.validate());

    o = swOptions();
    o.tolerance.before = Time{ -1, 30 };
    rejects (o, "negative before");
    o = swOptions();
    o.tolerance.after = Time{ -1, 1000 };
    rejects (o, "negative after");
    o = swOptions();
    o.tolerance.before = Time::negativeInfinity();
    rejects (o, "-inf");
    o = swOptions();
    o.tolerance.after = Time::invalid();
    rejects (o, "invalid");
    o = swOptions();
    o.videoStreamIndex = -5;
    rejects (o, "negative stream index");
    o = swOptions();
    o.maximumSize = stills::Size{ -1, 0 };
    rejects (o, "negative box");
    o = swOptions();
    o.decoderThreads = -1;
    rejects (o, "negative threads");
    o = swOptions (stills::PixelFormat::yuv420p);
    o.maximumSize = stills::Size{ 1, 1 };
    rejects (o, "1x1 box for 4:2:0");
    o = swOptions (stills::PixelFormat::nv12);
    o.maximumSize = stills::Size{ 0, 1 };
    rejects (o, "height 1 for nv12");
    o = swOptions();
    o.hardware.deviceType = stills::HardwareDeviceType::vaapi;
    rejects (o, "device type with softwareOnly");
    o = swOptions();
    o.hardware.device = "/dev/dri/renderD128";
    rejects (o, "device with softwareOnly");
    o = Options{};
    o.hardware.device = "/dev/dri/renderD128";
    rejects (o, "device without a type");
    o = swOptions();
    o.demuxerOptions = { { "", "x" } };
    rejects (o, "empty option key");

    // Accepted edge cases.
    o = swOptions (stills::PixelFormat::rgba);
    o.maximumSize = stills::Size{ 1, 1 };
    REQUIRE_OK (o.validate());
    auto tiny = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mp4").string(), o));
    auto img = REQUIRE_OK (tiny.imageAt (Time::zero()));
    CHECK (img.getSize() == stills::Size{ 1, 1 });
    o = swOptions();
    o.tolerance = stills::Tolerance::any();
    REQUIRE_OK (o.validate());
    o = swOptions();
    o.demuxerOptions = { { "probesize", "5000000" } };
    REQUIRE_OK (o.validate());
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mp4").string(), o));
    CHECK (frameIndexOf (REQUIRE_OK (g.imageAt (Time{ 9, 30 }))) == 9);
}

TEST_CASE ("options: filesystem::path overload", "[open]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter.mp4"), swOptions()));
    CHECK (frameIndexOf (REQUIRE_OK (g.imageAt (Time{ 2, 30 }))) == 2);
}

TEST_CASE ("interop: adoptFrame preconditions", "[image][interop]")
{
    // Accepts a properly allocated NV12 frame.
    AVFrame* nv12 = av_frame_alloc();
    nv12->format = AV_PIX_FMT_NV12;
    nv12->width = 8;
    nv12->height = 8;
    REQUIRE (av_frame_get_buffer (nv12, 0) == 0);
    nv12->pts = 3;
    auto ok = REQUIRE_OK (stills::interop::adoptFrame (nv12, stills::Rational{ 1, 30 }));
    CHECK (ok.getPlaneCount() == 2);
    CHECK (ok.getPlaneSize (1) == stills::Size{ 4, 4 });
    CHECK (ok.getActualTime() == Time{ 3, 30 });

    // Rejects a frame that does not own its pixels.
    std::vector<std::uint8_t> pixels (8 * 8 * 4);
    AVFrame* borrowed = av_frame_alloc();
    borrowed->format = AV_PIX_FMT_RGBA;
    borrowed->width = 8;
    borrowed->height = 8;
    borrowed->data[0] = pixels.data();
    borrowed->linesize[0] = 8 * 4;
    REQUIRE_ERROR (stills::interop::adoptFrame (borrowed), ErrorCode::invalidArgument);
    borrowed->data[0] = nullptr;
    av_frame_free (&borrowed);

    // Rejects a planar frame with a missing plane.
    AVFrame* missing = av_frame_alloc();
    missing->format = AV_PIX_FMT_YUV420P;
    missing->width = 8;
    missing->height = 8;
    REQUIRE (av_frame_get_buffer (missing, 0) == 0);
    std::uint8_t* saved = missing->data[2];
    missing->data[2] = nullptr;
    REQUIRE_ERROR (stills::interop::adoptFrame (missing), ErrorCode::invalidArgument);
    missing->data[2] = saved;
    av_frame_free (&missing);
}

TEST_CASE ("time: conveniences", "[time]")
{
    CHECK (Time::frames (29, { 30, 1 }) == Time{ 29, 30 });
    CHECK (Time::frames (29, { 30000, 1001 }) == Time{ 29 * 1001, 30000 });
    CHECK_FALSE (Time::frames (1, { 0, 1 }).isValid());
    std::unordered_set<Time> set{ Time{ 1, 2 }, Time{ 2, 4 }, Time{ 3, 6 }, Time::zero(), Time{ 0, 30 } };
    CHECK (set.size() == 2); // 1/2 == 2/4 == 3/6; 0 == 0/30
    CHECK (std::hash<Time>{}(Time{ 1, 2 }) == std::hash<Time>{}(Time{ 2, 4 }));
    CHECK (std::hash<Time>{}(Time::positiveInfinity()) != std::hash<Time>{}(Time::negativeInfinity()));
#if STILLS_HAS_FORMAT
    CHECK (std::format ("{}", Time{ 1, 2 }) == "0.500000s (1/2)");
    CHECK (std::format ("{}", stills::Rational{ 30, 1 }) == "30/1");
    CHECK (std::format ("{}", stills::Size{ 64, 48 }) == "64x48");
#endif
    CHECK (toString (stills::WaitResult::refusedOnWorkerThread) == "refusedOnWorkerThread");
}

// A container costs a few hundred bytes to declare an enormous frame, and open() allocates for it:
// the decoder is built and the first frame probed there, so a 8192x8192 declaration in a ~200 KB
// file reaches a gigabyte of resident memory before the caller can look at getInfo().codedSize.
TEST_CASE ("options: maxInputPixels refuses an oversized frame before the decoder is built", "[options][security]")
{
    const auto path = fixture ("huge8k.mp4").string();
    stills::Options capped = swOptions();
    capped.maxInputPixels = 8294400; // 4K
    auto refused = stills::AssetImageGenerator::open (path, capped);
    REQUIRE_FALSE (refused.has_value());
    CHECK (refused.error().code == stills::ErrorCode::unsupportedFormat);
    INFO (refused.error());
    CHECK (refused.error().message.find ("8192x8192") != std::string::npos);
    CHECK (refused.error().message.find ("maxInputPixels") != std::string::npos);

    // Without the cap the same file opens, which is why the cap has to be opt-in.
    auto allowed = stills::AssetImageGenerator::open (path, swOptions());
    REQUIRE (allowed.has_value());
    CHECK (allowed->getInfo().codedSize == stills::Size{ 8192, 8192 });

    // An ordinary clip is unaffected by a cap it fits under.
    auto ok = stills::AssetImageGenerator::open (fixture ("counter.mp4").string(), capped);
    REQUIRE (ok.has_value());

    // The cap itself is validated.
    stills::Options bad = swOptions();
    bad.maxInputPixels = 0;
    REQUIRE_ERROR (stills::AssetImageGenerator::open (fixture ("counter.mp4").string(), bad),
                   stills::ErrorCode::invalidArgument);
}

// The source string reaches every protocol libavformat has, so a typo in a security setting must
// not be silent, and the whitelist keys documented on Options::demuxerOptions have to work.
TEST_CASE ("options: untrusted sources are confined to the file protocol", "[options][security]")
{
    const auto clip = fixture ("counter.mp4").string();
    SECTION ("protocol_whitelist opens ordinary files and refuses concat:")
    {
        Options o = swOptions();
        o.demuxerOptions = { { "protocol_whitelist", "file" }, { "format_whitelist", "mov,mp4,m4a,matroska,webm" } };
        auto g = stills::AssetImageGenerator::open (clip, o);
        REQUIRE (g.has_value());
        // `concat:` is a protocol, not a path: without a whitelist it opens whatever it is given,
        // which is how a path-prefix check on the source is defeated.
        auto allowed = stills::AssetImageGenerator::open ("concat:" + clip, swOptions());
        CHECK (allowed.has_value());
        auto refused = stills::AssetImageGenerator::open ("concat:" + clip, o);
        CHECK_FALSE (refused.has_value());
    }

    SECTION ("a misspelled demuxer option is an error, not a silent no-op")
    {
        Options typo = swOptions();
        typo.demuxerOptions = { { "protocol_whitlist", "file" } };
        auto r = stills::AssetImageGenerator::open (clip, typo);
        REQUIRE_FALSE (r.has_value());
        CHECK (r.error().code == ErrorCode::invalidArgument);
        INFO (r.error());
        CHECK (r.error().message.find ("protocol_whitlist") != std::string::npos);
    }

    SECTION ("a real key that this source does not use is accepted")
    {
        // HTTP options are real libavformat keys; a local path consumes none of them. `reconnect`
        // is defined by the http protocol alone, so an FFmpeg built without it rejects the key as
        // unknown, which is correct and not what this section tests.
        bool hasHttp = false;
        void* it = nullptr;

        while (const char* name = avio_enum_protocols (&it, 0))
            if (std::string_view{ name } == "http") hasHttp = true;

        if (! hasHttp) SKIP ("this FFmpeg build has no http protocol");
        Options http = swOptions();
        http.demuxerOptions = { { "reconnect", "1" }, { "rw_timeout", "10000000" } };
        auto g = stills::AssetImageGenerator::open (clip, http);
        CHECK (g.has_value());
    }
}

// validate() used to check lower bounds only, so a typo like INT_MAX reached libavcodec and came
// back as "Cannot allocate memory" several calls later.
TEST_CASE ("options: validate rejects an out-of-range decoderThreads", "[options]")
{
    const auto clip = fixture ("counter.mp4").string();
    const auto rejected = [&] (auto tweak)
    {
        Options o = swOptions();
        tweak (o);
        auto r = AssetImageGenerator::open (clip, o);
        REQUIRE_FALSE (r.has_value());
        CHECK (r.error().code == ErrorCode::invalidArgument);
        INFO (r.error());
    };

    rejected ([] (Options& o) { o.decoderThreads = std::numeric_limits<int>::max(); });
    rejected ([] (Options& o) { o.decoderThreads = Options::maxDecoderThreads + 1; });
    // The bounds themselves are accepted.
    Options ok = swOptions();
    ok.decoderThreads = 2;
    CHECK (AssetImageGenerator::open (clip, ok).has_value());
}

// ErrorCode separates "it is broken" from "position it first", and can travel as a
// std::error_code.
TEST_CASE ("options: ErrorCode converts to std::error_code", "[errors]")
{
    const std::error_code ec = stills::ErrorCode::timeOutOfRange;
    CHECK (ec.category() == stills::getErrorCategory());
    CHECK (ec.category().name() == std::string{ "stills" });
    CHECK (ec.message() == "timeOutOfRange");
    CHECK (ec == stills::ErrorCode::timeOutOfRange);
    CHECK (ec != stills::ErrorCode::invalidState);
    CHECK (stills::make_error_code (stills::ErrorCode::invalidState).value()
           == static_cast<int> (stills::ErrorCode::invalidState));
    CHECK (toString (stills::ErrorCode::invalidState) == "invalidState");
}

// A source built from a fixed buffer can carry an embedded NUL; libavformat would have seen only
// the prefix and opened that instead, with no sign anything was dropped.
TEST_CASE ("options: a source with an embedded NUL is rejected", "[open]")
{
    const auto clip = fixture ("counter.mp4").string();
    const std::string_view truncating{ "\0/etc/passwd", 12 };
    std::string withNul = clip;
    withNul.push_back ('\0');
    withNul += "and-more";
    REQUIRE_ERROR (AssetImageGenerator::open (std::string_view{ withNul }, swOptions()), ErrorCode::invalidArgument);
    REQUIRE_ERROR (AssetImageGenerator::open (truncating, swOptions()), ErrorCode::invalidArgument);
    REQUIRE_ERROR (AssetImageGenerator::open (std::string_view{}, swOptions()), ErrorCode::invalidArgument);
    CHECK (AssetImageGenerator::open (clip, swOptions()).has_value()); // the same path without one
}

// close() releases the asset where the scope cannot, and cancelAll() cancels a queued batch.
TEST_CASE ("options: close() and cancelAll()", "[api]")
{
    auto g = openCounter();
    Collector col;
    std::vector<Time> times;

    for (int n = 0; n < 40; ++n)
        times.emplace_back (n * 3, 30);
    auto req = g.generateImages (times, col.handler());
    g.cancelAll();
    REQUIRE (req.wait() == stills::WaitResult::finished);
    CHECK (col.count() == times.size());
    CHECK (col.count (stills::GenerationStatus::cancelled) > 0);

    CHECK (g.isOpen());
    g.close();
    CHECK_FALSE (g.isOpen());
    g.close(); // idempotent
    REQUIRE_ERROR (g.imageAt (Time{ 1, 30 }), ErrorCode::invalidState);
}

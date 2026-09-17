#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::Time;

TEST_CASE ("open: failure modes map to distinct error codes", "[open]")
{
    SECTION ("missing file")
    {
        auto g = AssetImageGenerator::open (fixture ("does_not_exist.mp4").string(), swOptions());
        REQUIRE_ERROR (g, ErrorCode::fileNotFound);
        REQUIRE (g.error().avError == AVERROR (ENOENT));
        REQUIRE (toString (g.error()).find ("avformat_open_input") != std::string::npos);
    }

    SECTION ("not a media file")
    {
        REQUIRE_ERROR (AssetImageGenerator::open (fixture ("not_a_video.mp4").string(), swOptions()),
                       ErrorCode::unsupportedFormat);
    }

    SECTION ("audio only")
    {
        REQUIRE_ERROR (AssetImageGenerator::open (fixture ("audio_only.m4a").string(), swOptions()),
                       ErrorCode::noVideoStream);
    }

    SECTION ("empty source")
    {
        REQUIRE_ERROR (AssetImageGenerator::open ("", swOptions()), ErrorCode::invalidArgument);
    }

    SECTION ("bad options")
    {
        stills::Options o = swOptions();
        o.maximumSize = stills::Size{ -1, 0 };
        REQUIRE_ERROR (AssetImageGenerator::open (fixture ("counter.mp4").string(), o), ErrorCode::invalidArgument);
        o = swOptions();
        o.tolerance.before = Time::invalid();
        REQUIRE_ERROR (AssetImageGenerator::open (fixture ("counter.mp4").string(), o), ErrorCode::invalidArgument);
        o = swOptions();
        o.videoStreamIndex = 7;
        REQUIRE_ERROR (AssetImageGenerator::open (fixture ("counter.mp4").string(), o), ErrorCode::invalidArgument);
    }
}

TEST_CASE ("open: asset info for the primary fixture", "[open]")
{
    auto g = openCounter();
    const auto& info = g.getInfo();
    CHECK (info.containerName.find ("mp4") != std::string::npos);
    CHECK (info.codecName == "h264");
    CHECK (info.sourcePixelFormat == "yuv420p");
    CHECK (info.videoStreamIndex == 0);
    CHECK (info.timeBase == stills::Rational{ 1, 15360 });
    CHECK (info.averageFrameRate == stills::Rational{ 30, 1 });
    REQUIRE (info.duration.has_value());
    CHECK (*info.duration == Time{ 4, 1 });
    REQUIRE (info.frameCount.has_value());
    CHECK (*info.frameCount == 120);
    CHECK (info.codedSize == stills::Size{ 64, 48 });
    CHECK (info.displaySize == stills::Size{ 64, 48 });
    CHECK (info.outputSize == stills::Size{ 64, 48 });
    CHECK (info.rotationDegrees == 0);
    REQUIRE (info.getTimeRange().has_value());
    CHECK (info.getTimeRange()->contains (Time{ 119, 30 }));
    CHECK_FALSE (info.getTimeRange()->contains (Time{ 4, 1 }));
    CHECK_FALSE (g.getActiveDecoder().hardware);
    CHECK (g.getActiveDecoder().decoderName == "h264");
    CHECK (g.getOptions().hardware.policy == stills::HardwarePolicy::softwareOnly);
}

TEST_CASE ("open: MPEG-TS with a non-zero start time is asset-relative", "[open][sync]")
{
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_offset.ts").string(), swOptions()));
    CHECK (g.getInfo().containerName == "mpegts");
    CHECK (g.getInfo().timeBase == stills::Rational{ 1, 90000 });
    REQUIRE (g.getInfo().duration.has_value());
    CHECK (g.getInfo().duration->toSeconds() > 3.9);

    for (int n : { 0, 1, 30, 45, 119 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        CHECK (frameIndexOf (img) == n);
        CHECK (img.getActualTime() == Time{ n, 30 });
    }
}

TEST_CASE ("open: a single PNG is a one-frame asset", "[open][sync]")
{
    auto g =
        REQUIRE_OK (AssetImageGenerator::open (fixture ("red.png").string(), swOptions (stills::PixelFormat::rgba)));
    CHECK (g.getInfo().codecName == "png");
    CHECK (g.getInfo().codedSize == stills::Size{ 32, 32 });
    auto img = REQUIRE_OK (g.imageAt (Time::zero()));
    CHECK (img.getSize() == stills::Size{ 32, 32 });
    const auto px = img.getPixels();
    CHECK (std::to_integer<int> (px[0]) > 240);  // R
    CHECK (std::to_integer<int> (px[1]) < 16);   // G
    CHECK (std::to_integer<int> (px[2]) < 16);   // B
    CHECK (std::to_integer<int> (px[3]) == 255); // A
    REQUIRE_ERROR (g.imageAt (Time{ 1, 1 }), ErrorCode::timeOutOfRange);
}

TEST_CASE ("open: anamorphic content is squared by default", "[open][sar]")
{
    SECTION ("applied")
    {
        auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_sar2.mp4").string(), swOptions()));
        CHECK (g.getInfo().codedSize == stills::Size{ 64, 48 });
        CHECK (g.getInfo().displaySize == stills::Size{ 128, 48 });
        CHECK (g.getInfo().outputSize == stills::Size{ 128, 48 });
        auto img = REQUIRE_OK (g.imageAt (Time{ 41, 30 }));
        CHECK (img.getSize() == stills::Size{ 128, 48 });
        CHECK (frameIndexOf (img) == 41);
    }

    SECTION ("with a box")
    {
        stills::Options o = swOptions (stills::PixelFormat::rgba);
        o.maximumSize = stills::Size{ 64, 64 };
        auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_sar2.mp4").string(), o));
        CHECK (g.getInfo().outputSize == stills::Size{ 64, 24 });
        auto img = REQUIRE_OK (g.imageAt (Time{ 41, 30 }));
        CHECK (img.getSize() == stills::Size{ 64, 24 });
        CHECK (frameIndexOf (img) == 41);
    }

    SECTION ("disabled")
    {
        stills::Options o = swOptions();
        o.applySampleAspectRatio = false;
        auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_sar2.mp4").string(), o));
        CHECK (g.getInfo().displaySize == stills::Size{ 64, 48 });
        auto img = REQUIRE_OK (g.imageAt (Time{ 41, 30 }));
        CHECK (img.getSize() == stills::Size{ 64, 48 });
    }
}

TEST_CASE ("open: 180 and 270 degree rotations", "[open][rotation]")
{
    // ffmpeg's -display_rotation is counter-clockwise; rotationDegrees is the clockwise rotation
    // needed for upright display, so a 270° CCW display matrix reports as 90.
    struct Case
    {
        const char* file;
        int degrees;
        stills::Size size;
    };

    const Case c = GENERATE (Case{ "counter_rot180.mp4", 180, stills::Size{ 64, 48 } },
                             Case{ "counter_rot270.mp4", 90, stills::Size{ 48, 64 } });
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture (c.file).string(), swOptions (stills::PixelFormat::rgb24)));
    CHECK (g.getInfo().rotationDegrees == c.degrees);
    CHECK (g.getInfo().displaySize == c.size);

    for (int n : { 0, 11, 77 })
    {
        auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
        CHECK (img.getSize() == c.size);
        CHECK (frameIndexOf (img, c.degrees) == n);
    }
}

TEST_CASE ("open: attached pictures are not video unless asked for", "[open]")
{
    REQUIRE_ERROR (AssetImageGenerator::open (fixture ("cover.m4a").string(), swOptions()), ErrorCode::noVideoStream);
    stills::Options o = swOptions (stills::PixelFormat::rgba);
    o.allowAttachedPictures = true;
    auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("cover.m4a").string(), o));
    CHECK (g.getInfo().codecName == "png");
    CHECK (g.getInfo().codedSize == stills::Size{ 32, 32 });
    auto img = REQUIRE_OK (g.imageAt (Time::zero()));
    CHECK (std::to_integer<int> (img.getPixels()[0]) > 240); // red cover art
}

TEST_CASE ("open: display-matrix rotation is applied by default", "[open][rotation]")
{
    SECTION ("applied")
    {
        auto g = REQUIRE_OK (
            AssetImageGenerator::open (fixture ("counter_rot90.mp4").string(), swOptions (stills::PixelFormat::rgba)));
        CHECK (g.getInfo().rotationDegrees == 270); // 90° counter-clockwise display matrix
        CHECK (g.getInfo().codedSize == stills::Size{ 64, 48 });
        CHECK (g.getInfo().displaySize == stills::Size{ 48, 64 });
        CHECK (g.getInfo().outputSize == stills::Size{ 48, 64 });

        for (int n : { 0, 7, 31, 100 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
            CHECK (img.getSize() == stills::Size{ 48, 64 });
            CHECK (frameIndexOf (img, g.getInfo().rotationDegrees) == n);
        }
    }

    SECTION ("not applied when disabled")
    {
        stills::Options o = swOptions (stills::PixelFormat::yuv420p);
        o.applyPreferredTrackTransform = false;
        auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_rot90.mp4").string(), o));
        CHECK (g.getInfo().displaySize == stills::Size{ 64, 48 });
        auto img = REQUIRE_OK (g.imageAt (Time{ 31, 30 }));
        CHECK (img.getSize() == stills::Size{ 64, 48 });
        CHECK (frameIndexOf (img) == 31);
    }

    SECTION ("rotation with a fitted box applies to the rotated output")
    {
        stills::Options o = swOptions (stills::PixelFormat::rgb24);
        o.maximumSize = stills::Size{ 24, 24 };
        auto g = REQUIRE_OK (AssetImageGenerator::open (fixture ("counter_rot90.mp4").string(), o));
        CHECK (g.getInfo().outputSize == stills::Size{ 18, 24 });
        auto img = REQUIRE_OK (g.imageAt (Time{ 5, 30 }));
        CHECK (img.getSize() == stills::Size{ 18, 24 });
        CHECK (frameIndexOf (img, g.getInfo().rotationDegrees) == 5);
    }
}

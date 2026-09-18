#pragma once
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <stills/stills_Stills.h>

#include "support/stills_TestCollector.h"
#include "support/stills_TestExpect.h"
#include "support/stills_TestFixtures.h"
#include "support/stills_TestFrameIndex.h"

namespace testsupport
{

/// Software-only options with the given output format. One decoder thread keeps the suite
/// deterministic and race-detector friendly; the library default is libavcodec's automatic count.
inline stills::Options swOptions (stills::PixelFormat fmt = stills::PixelFormat::yuv420p)
{
    stills::Options o;
    o.hardware.policy = stills::HardwarePolicy::softwareOnly;
    o.pixelFormat = fmt;
    o.decoderThreads = 1;
    return o;
}

/// A generator over counter.mp4 (software, yuv420p by default).
inline stills::AssetImageGenerator openCounter (stills::Options o = swOptions())
{
    auto g = stills::AssetImageGenerator::open (fixture ("counter.mp4").string(), std::move (o));

    if (! g) FAIL ("open(counter.mp4) failed: " << g.error());
    return std::move (*g);
}

/// Quiet libav once per test binary.
struct QuietLogs
{
    QuietLogs() { stills::setLogLevel (stills::LogLevel::quiet); }
};

inline const QuietLogs quietLogs{};

} // namespace testsupport

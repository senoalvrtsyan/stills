#include "support/stills_TestCommon.h"

using namespace testsupport;
using stills::PixelFormat;
using stills::Time;

TEST_CASE ("determinism: repeated software extraction is byte-identical", "[determinism]")
{
    std::vector<std::vector<std::byte>> runs;

    for (int run = 0; run < 3; ++run)
    {
        auto g = openCounter (swOptions (PixelFormat::rgba));
        std::vector<std::byte> all;

        for (int n : { 0, 17, 29, 30, 64, 99, 119, 3 })
        {
            auto img = REQUIRE_OK (g.imageAt (Time{ n, 30 }));
            const auto bytes = REQUIRE_OK (img.toPackedBytes());
            all.insert (all.end(), bytes.begin(), bytes.end());
        }

        runs.push_back (std::move (all));
    }

    CHECK (runs[0] == runs[1]);
    CHECK (runs[1] == runs[2]);
}

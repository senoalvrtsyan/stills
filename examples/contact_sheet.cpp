// contact_sheet <input> <cols> <rows> <out.ppm> [cell_width]
// Asynchronous batch extraction of cols*rows evenly spaced frames, tiled into one PPM.
// Ctrl-C cancels the batch; every pending frame is then reported as cancelled.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stills/stills_Stills.h>
#include <string>
#include <vector>

#include "ppm.hpp"

namespace
{
std::atomic<bool> interrupted{ false };
void onSigint (int)
{
    interrupted.store (true);
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 5)
    {
        std::cerr << "usage: contact_sheet <input> <cols> <rows> <out.ppm> [cell_width]\n";
        return 2;
    }

    stills::setLogLevel (stills::LogLevel::error);
    const int cols = std::atoi (argv[2]);
    const int rows = std::atoi (argv[3]);
    const int cellW = argc > 5 ? std::atoi (argv[5]) : 160;

    stills::Options options;
    options.pixelFormat = stills::PixelFormat::rgb24;
    options.maximumSize = stills::Size{ cellW, 0 };
    options.tolerance = stills::Tolerance::exact();

    auto generator = stills::AssetImageGenerator::open (argv[1], options);

    if (! generator)
    {
        std::cerr << "open failed: " << generator.error() << '\n';
        return 1;
    }

    if (! generator->getInfo().duration)
    {
        std::cerr << "asset duration unknown; cannot space frames\n";
        return 1;
    }

    const stills::Size cell = generator->getInfo().outputSize;
    const int sheetW = cell.width * cols;
    const int sheetH = cell.height * rows;
    std::vector<std::byte> sheet (static_cast<std::size_t> (sheetW) * static_cast<std::size_t> (sheetH) * 3);

    const int count = cols * rows;
    const double duration = generator->getInfo().duration->toSeconds();
    std::vector<stills::Time> times;

    for (int i = 0; i < count; ++i)
        times.push_back (stills::Time::seconds (duration * (i + 0.5) / count));

    std::mutex io;
    int ok = 0;
    int failed = 0;
    int cancelled = 0;
    auto request = generator->generateImages (
        times,
        [&] (stills::Completion c)
        {
            std::lock_guard lk (io);
            switch (c.getStatus())
            {
            case stills::GenerationStatus::succeeded:
            {
                ++ok;
                const int slot = static_cast<int> (c.index); // position in `times`
                const stills::Image& img = *c.result;
                const int x0 = (slot % cols) * cell.width;
                const int y0 = (slot / cols) * cell.height;

                for (int y = 0; y < img.getHeight(); ++y)
                {
                    const std::byte* src = img.getPixels().data() + static_cast<std::size_t> (y) * img.getRowStride (0);
                    std::byte* dst = sheet.data()
                                     + (static_cast<std::size_t> (y0 + y) * static_cast<std::size_t> (sheetW)
                                        + static_cast<std::size_t> (x0))
                                           * 3;
                    std::memcpy (dst, src, static_cast<std::size_t> (img.getWidth()) * 3);
                }

                std::cout << "  " << c.requestedTime << " -> " << img.getActualTime() << '\n';
                break;
            }

            case stills::GenerationStatus::failed:
                ++failed;
                std::cerr << "  " << c.requestedTime << " failed: " << c.result.error() << '\n';
                break;
            case stills::GenerationStatus::cancelled:
                ++cancelled;
                break;
            }
        });

    std::signal (SIGINT, onSigint);

    while (request.waitFor (std::chrono::milliseconds{ 50 }) != stills::WaitResult::finished)
    {
        if (interrupted.load())
        {
            std::cerr << "interrupted: cancelling\n";
            generator->cancelAll();
        }
    }

    std::cout << ok << " ok, " << failed << " failed, " << cancelled << " cancelled\n";

    if (ok == 0) return 1;
    if (! writePpm (argv[4], sheetW, sheetH, static_cast<std::size_t> (sheetW) * 3, sheet))
    {
        std::cerr << "could not write " << argv[4] << '\n';
        return 1;
    }

    return 0;
}

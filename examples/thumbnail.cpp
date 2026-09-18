// thumbnail <input> <seconds> <out.ppm> [max_width]
// Synchronous extraction of one frame, written as a PPM.
#include <cstdlib>
#include <iostream>
#include <stills/stills_Stills.h>
#include <string>

#include "stills_Ppm.h"

int main (int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: thumbnail <input> <seconds> <out.ppm> [max_width]\n";
        return 2;
    }

    stills::setLogLevel (stills::LogLevel::error);

    stills::Options options;
    options.pixelFormat = stills::PixelFormat::rgb24;

    if (argc > 4) options.maximumSize = stills::Size{ std::atoi (argv[4]), 0 };

    auto generator = stills::AssetImageGenerator::open (argv[1], options);

    if (! generator)
    {
        std::cerr << "open failed: " << generator.error() << '\n';
        return 1;
    }

    const auto& info = generator->getInfo();
    std::cout << info.codecName << ' ' << info.codedSize << " -> " << info.outputSize << ", duration "
              << (info.duration ? toString (*info.duration) : "unknown") << ", decoder "
              << (generator->getActiveDecoder().hardware ? "hardware" : "software") << '\n';

    auto image = generator->imageAt (stills::Time::seconds (std::atof (argv[2])));

    if (! image)
    {
        std::cerr << "extraction failed: " << image.error() << '\n';
        return 1;
    }

    std::cout << "frame at " << image->getActualTime() << (image->isKeyframe() ? " (keyframe)" : "")
              << (image->wasClamped() ? " (clamped)" : "") << '\n';

    if (! writePpm (argv[3], image->getWidth(), image->getHeight(), image->getRowStride (0), image->getPixels()))
    {
        std::cerr << "could not write " << argv[3] << '\n';
        return 1;
    }

    return 0;
}

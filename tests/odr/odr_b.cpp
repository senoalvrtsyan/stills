#include <stills/stills_Interop.h>
#include <stills/stills_Stills.h>
#include <string>
int odrOpenB (const std::string& path)
{
    auto g =
        stills::AssetImageGenerator::open (path, { .hardware = { .policy = stills::HardwarePolicy::softwareOnly } });

    if (! g) return 1;
    auto img = g->imageAt (stills::Time{ 2, 30 });

    if (! img) return 2;
    return stills::interop::getNativeFrame (*img) != nullptr ? 0 : 3;
}

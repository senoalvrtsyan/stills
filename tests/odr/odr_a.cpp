#include <stills/stills_Stills.h>
#include <string>
int odrOpenA (const std::string& path)
{
    auto g =
        stills::AssetImageGenerator::open (path, { .hardware = { .policy = stills::HardwarePolicy::softwareOnly } });

    if (! g) return 1;
    auto img = g->imageAt (stills::Time{ 1, 30 });
    return img ? 0 : 2;
}

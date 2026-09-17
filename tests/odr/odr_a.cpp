#include <stills/stills.hpp>
#include <string>
int odr_open_a(const std::string& path) {
  auto g = stills::AssetImageGenerator::open(
      path, {.hardware = {.policy = stills::HardwarePolicy::software_only}});
  if (!g) return 1;
  auto img = g->image_at(stills::Time{1, 30});
  return img ? 0 : 2;
}

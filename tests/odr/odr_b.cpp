#include <stills/interop.hpp>
#include <stills/stills.hpp>
#include <string>
int odr_open_b(const std::string& path) {
  auto g = stills::AssetImageGenerator::open(
      path, {.hardware = {.policy = stills::HardwarePolicy::software_only}});
  if (!g) return 1;
  auto img = g->image_at(stills::Time{2, 30});
  if (!img) return 2;
  return stills::interop::native_frame(*img) != nullptr ? 0 : 3;
}

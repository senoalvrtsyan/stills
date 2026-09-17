#include "support/common.hpp"

using namespace testsupport;
using stills::PixelFormat;
using stills::Time;

TEST_CASE("determinism: repeated software extraction is byte-identical", "[determinism]") {
  std::vector<std::vector<std::byte>> runs;
  for (int run = 0; run < 3; ++run) {
    auto g = open_counter(sw_options(PixelFormat::rgba));
    std::vector<std::byte> all;
    for (int n : {0, 17, 29, 30, 64, 99, 119, 3}) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      const auto bytes = REQUIRE_OK(img.to_packed_bytes());
      all.insert(all.end(), bytes.begin(), bytes.end());
    }
    runs.push_back(std::move(all));
  }
  CHECK(runs[0] == runs[1]);
  CHECK(runs[1] == runs[2]);
}

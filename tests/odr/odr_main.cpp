#include <string>

int odr_open_a(const std::string& path);
int odr_open_b(const std::string& path);

// Two translation units that each drive the library, linked with a third that calls both: this is
// what catches a namespace-scope entity in the headers that is not `inline`, which is the only way
// "header-only" can quietly stop being true.
int main() {
  const std::string p = std::string{STILLS_FIXTURE_DIR} + "/counter.mp4";
  return odr_open_a(p) + odr_open_b(p);
}

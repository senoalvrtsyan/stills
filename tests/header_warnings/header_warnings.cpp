// Every public header, seen through a plain -I include path under the project's strict warning set.
// The library's own targets see the headers as SYSTEM, which silences warnings inside them; this TU
// is where a warning in our own code fails the build (tests/CMakeLists.txt).
#include <stills/stills_Interop.h>
#include <stills/stills_Stills.h>

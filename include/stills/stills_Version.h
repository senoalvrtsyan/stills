#pragma once
// stills/stills_Version.h — the library's version, for a consumer that has to branch on it.
//
// Kept in the source tree rather than generated, so the bare `-I include` route works without a
// build step. Semantics until 1.0: the minor version is the compatibility unit — every 0.x.y with
// the same x is source-compatible, and a bump of x may break source compatibility.

#include <string_view>

#define STILLS_VERSION_MAJOR 0
#define STILLS_VERSION_MINOR 1
#define STILLS_VERSION_PATCH 0
#define STILLS_VERSION_STRING "0.1.0"

/// Comparable as a single number: STILLS_VERSION >= STILLS_VERSION_AT_LEAST(0, 1, 0).
#define STILLS_VERSION_AT_LEAST(major, minor, patch) ((major) * 10000 + (minor) * 100 + (patch))
#define STILLS_VERSION STILLS_VERSION_AT_LEAST (STILLS_VERSION_MAJOR, STILLS_VERSION_MINOR, STILLS_VERSION_PATCH)

namespace stills
{

inline constexpr int versionMajor = STILLS_VERSION_MAJOR;
inline constexpr int versionMinor = STILLS_VERSION_MINOR;
inline constexpr int versionPatch = STILLS_VERSION_PATCH;
inline constexpr std::string_view version = STILLS_VERSION_STRING;

} // namespace stills

#pragma once
// stills/detail/stills_Config.h — the toolchain gate, included first by every header that needs it so a
// compiler that cannot build this library says so in one line instead of several hundred.
//
// cxx_std_23 alone is not enough: GCC 11 and 12 advertise it and have no <expected>. The test has to
// be `#if defined` rather than a static_assert on the macro, because an undefined macro in a
// static_assert is itself an undeclared-identifier error, inside the wall it was meant to prevent.

#include <version>

#if ! defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error                                                                                                                 \
    "stills requires C++23 with <expected>: GCC 13+, or Clang 17+ with libc++ 16+ (-stdlib=libc++). Compile with -std=c++23. GCC 11/12 advertise C++23 but do not provide <expected>, and Clang against libstdc++ does not see it either. MSVC is not supported."
#endif

#if ! defined(__SIZEOF_INT128__)
#error "stills requires a compiler with __int128 support (GCC or Clang)."
#endif

// Whether the std::formatter specialisations for the library's enums and value types are defined.
// The feature-test macro alone is not enough: libc++ shipped a usable <format> in 17 but withheld
// __cpp_lib_format until the whole facility was complete, and Xcode's libc++ does the same, so a
// test of the macro alone would silently leave a Clang consumer without the formatters.
#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#define STILLS_HAS_FORMAT 1
#elif defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 170000 && __has_include(<format>)
#define STILLS_HAS_FORMAT 1
#else
#define STILLS_HAS_FORMAT 0
#endif

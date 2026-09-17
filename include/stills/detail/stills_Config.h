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

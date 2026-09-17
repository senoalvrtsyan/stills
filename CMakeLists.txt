cmake_minimum_required(VERSION 3.25)

project(stills
  VERSION 0.1.0
  DESCRIPTION "Header-only C++23 AVAssetImageGenerator over FFmpeg"
  LANGUAGES CXX)

option(STILLS_BUILD_TESTS    "Build the stills test-suite" ${PROJECT_IS_TOP_LEVEL})
option(STILLS_BUILD_EXAMPLES "Build the stills examples"   ${PROJECT_IS_TOP_LEVEL})
option(STILLS_INSTALL        "Generate the stills install rules" ${PROJECT_IS_TOP_LEVEL})
option(STILLS_WERROR         "Treat warnings as errors"    ON)

# The library itself asks for C++23 through target_compile_features, which is all a consumer needs.
# A top-level build also has to pin the standard globally, because it builds code we do not own:
# the fetched Catch2 asks only for cxx_std_14, so CMake emits no -std flag for it and it builds at
# the compiler's default while our TUs are at C++23. Catch2 gates part of its interface on the
# language level -- StringMaker<std::string_view>, <std::wstring_view> and <std::byte> are declared
# in catch_tostring.hpp and defined in catch_tostring.cpp under CATCH_CONFIG_CPP17_*, so a C++23 TU
# against a Catch2 built below C++17 gets undefined references (reproduced here with GCC 13). That
# is the shape of the reported Apple Clang 17 failure, but not a verified diagnosis of it: at
# C++17-vs-C++23 the strong-symbol surface is identical on libstdc++ and the link succeeds, and
# there is no Apple toolchain here. Pinning both sides removes the class of failure either way.
# Guarded on PROJECT_IS_TOP_LEVEL: as a subproject these variables belong to the consumer.
# EXTENSIONS OFF is safe for us: stills_Time.h already spells its one extension `__extension__ using
# int128 = __int128;` rather than relying on -std=gnu++23.
if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

find_package(PkgConfig REQUIRED)
# IMPORTED_TARGET_GLOBAL: the imported target is otherwise scoped to this directory, so an
# add_subdirectory() consumer of <stills/stills_Interop.h> cannot link PkgConfig::FFMPEG itself.
# Lower bounds, matching the static_asserts in detail/stills_FFmpeg.h (FFmpeg 6.1). Keep them in step:
# with only the major versions here, FFmpeg 6.0 configures cleanly and then fails mid-compile on a
# static_assert, which is a much worse error than one from pkg-config.
# There is deliberately no upper bound, and the headers no longer impose one either. The
# headers used to impose one -- an #error on a libavformat major the AVIOContext recovery had not
# been verified against, on a path only a cancelled request reaches -- which pkg-config could not
# see, so a newer FFmpeg configured cleanly and then died mid-compile. That guard is now
# tryResetIoState() in detail/stills_MediaSource.h: above the verified range it returns false and the
# caller re-opens the source, so a new major builds and costs one re-open per interrupt recovery on
# a rewindable source. On a pipe there is nothing to re-open and the stuck read still fails -- the
# same outcome as before, minus the build failure.
# The verified upper end lives in that one #if and is stated in the README; it is not a build
# requirement, so it is not enforced here.
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
  libavformat>=60.16
  libavcodec>=60.31
  libavutil>=58.29
  libswscale>=7)
# Nothing marks libav's headers SYSTEM here, deliberately: they already reach our targets that way,
# for two reasons this file does not control. CMake treats an IMPORTED target's include directories
# as SYSTEM by default, transitively through `stills`; and where pkg-config names a directory the
# compiler already searches implicitly, CMake emits no include flag for it at all -- which is what
# happens on this machine. So the -Werror build does not depend on the diagnostic pragmas in
# detail/stills_FFmpeg.h. Those are for consumers who reach FFmpeg through a plain -I.
find_package(Threads REQUIRED)

include(GNUInstallDirs)

add_library(stills INTERFACE)
add_library(stills::stills ALIAS stills)
# SYSTEM: without it an add_subdirectory()/FetchContent consumer with its own style warnings sees
# them inside our headers.
target_include_directories(stills SYSTEM INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(stills INTERFACE cxx_std_23)
target_link_libraries(stills INTERFACE PkgConfig::FFMPEG Threads::Threads)

include(cmake/stills_warnings.cmake)

# ---- install / export --------------------------------------------------------------------------
# Header-only: the "library" is the headers plus the usage requirements recorded on the INTERFACE
# target, so installing is the headers, an exported target, and the config that re-finds FFmpeg.
if(STILLS_INSTALL)
  include(CMakePackageConfigHelpers)
  set(STILLS_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/stills)

  install(TARGETS stills EXPORT stillsTargets)
  install(DIRECTORY include/stills DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT stillsTargets
    FILE stillsTargets.cmake
    NAMESPACE stills::
    DESTINATION ${STILLS_CMAKE_DIR})

  configure_package_config_file(cmake/stillsConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/stillsConfig.cmake
    INSTALL_DESTINATION ${STILLS_CMAKE_DIR})
  # SameMinorVersion: until 1.0 the minor version is the compatibility unit (stills_Version.h).
  # ARCH_INDEPENDENT: no compiled artefact, so a 32-bit consumer may use a 64-bit install.
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/stillsConfigVersion.cmake
    COMPATIBILITY SameMinorVersion
    ARCH_INDEPENDENT)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/stillsConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/stillsConfigVersion.cmake
    DESTINATION ${STILLS_CMAKE_DIR})
endif()

if(STILLS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(STILLS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

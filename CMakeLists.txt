cmake_minimum_required(VERSION 3.25)

project(stills
  VERSION 0.1.0
  DESCRIPTION "Header-only C++23 AVAssetImageGenerator over FFmpeg"
  LANGUAGES CXX)

option(STILLS_BUILD_TESTS    "Build the stills test-suite" ${PROJECT_IS_TOP_LEVEL})
option(STILLS_BUILD_EXAMPLES "Build the stills examples"   ${PROJECT_IS_TOP_LEVEL})
option(STILLS_INSTALL        "Generate the stills install rules" ${PROJECT_IS_TOP_LEVEL})
option(STILLS_WERROR         "Treat warnings as errors"    ON)

find_package(PkgConfig REQUIRED)
# IMPORTED_TARGET_GLOBAL: the imported target is otherwise scoped to this directory, so an
# add_subdirectory() consumer of <stills/interop.hpp> cannot link PkgConfig::FFMPEG itself.
# The bounds match the static_asserts in detail/ffmpeg.hpp (FFmpeg 6.1). Keep them in step: with
# only the major versions here, FFmpeg 6.0 configures cleanly and then fails mid-compile on a
# static_assert, which is a much worse error than one from pkg-config.
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
  libavformat>=60.16
  libavcodec>=60.31
  libavutil>=58.29
  libswscale>=7)
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
  # SameMinorVersion: until 1.0 the minor version is the compatibility unit (version.hpp).
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

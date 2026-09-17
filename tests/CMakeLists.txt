include(FetchContent)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.16.0
  GIT_SHALLOW    TRUE
  SYSTEM
  FIND_PACKAGE_ARGS 3)
FetchContent_MakeAvailable(Catch2)
# Catch2 asks for cxx_std_14 only, so left alone it builds at the compiler's default while our TUs
# are at C++23 -- the mismatch behind the reported Apple Clang 17 link failure. A top-level build
# already pins CMAKE_CXX_STANDARD; this covers the consumer who builds our tests as a subproject.
# Guarded, because with FIND_PACKAGE_ARGS above a system Catch2 wins and exports only namespaced
# IMPORTED targets (Catch2::Catch2*), so set_target_properties(Catch2 ...) is a hard CMake error
# ("Can not find target to add properties to") -- and would do nothing on an IMPORTED target
# anyway. See the README caveat.
if(TARGET Catch2 AND TARGET Catch2WithMain)
  set_target_properties(Catch2 Catch2WithMain PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
endif()
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ---- fixtures -------------------------------------------------------------------------------
# Every clip the suite reads is generated from `fixtures/generate.sh` at build time, so a checkout
# plus an ffmpeg binary is all a reviewer needs.
find_program(FFMPEG_EXECUTABLE ffmpeg REQUIRED)
set(STILLS_FIXTURE_DIR ${CMAKE_BINARY_DIR}/fixtures)
set(STILLS_FIXTURE_FILES counter.mp4 counter_offset.ts counter_longgop.ts counter.mkv
                         counter_rot90.mp4 counter_rot180.mp4 counter_rot270.mp4
                         counter_hflip.mp4 counter_vflip.mp4 counter_editlist.mp4 av_audio_first.mp4
                         counter_vfr.mp4 counter_sar2.mp4 counter.h264 audio_only.m4a cover.m4a red.png
                         alpha.mov odd.mov counter_corrupt.mp4 counter_trunc.mp4 not_a_video.mp4
                         counter_frag.mp4 counter_profile_switch.ts counter_p10.mp4 counter_opengop.mp4
                         counter_ilace.mp4 counter.mjpeg odd_rot90.mov counter_corrupt_fixed.mp4
                         counter_vfr_hold.mp4 disc.ts huge8k.mp4)
# VP9/WebM needs libvpx in the ffmpeg binary; the test skips itself when the fixture is absent.
execute_process(COMMAND ${FFMPEG_EXECUTABLE} -hide_banner -encoders
                OUTPUT_VARIABLE STILLS_FFMPEG_ENCODERS ERROR_QUIET)
if(STILLS_FFMPEG_ENCODERS MATCHES "libvpx-vp9")
  list(APPEND STILLS_FIXTURE_FILES counter_vp9.webm)
  set(STILLS_HAVE_VP9 1)
else()
  set(STILLS_HAVE_VP9 0)
  message(STATUS "stills: ffmpeg has no libvpx-vp9 encoder; the WebM test will be skipped")
endif()
# open-GOP HEVC needs libx265; the test skips itself when the fixture is absent.
if(STILLS_FFMPEG_ENCODERS MATCHES "libx265")
  list(APPEND STILLS_FIXTURE_FILES counter_opengop_hevc.mp4)
  set(STILLS_HAVE_X265 1)
else()
  set(STILLS_HAVE_X265 0)
  message(STATUS "stills: ffmpeg has no libx265 encoder; the open-GOP HEVC test will be skipped")
endif()
list(TRANSFORM STILLS_FIXTURE_FILES PREPEND ${STILLS_FIXTURE_DIR}/ OUTPUT_VARIABLE STILLS_FIXTURE_PATHS)
add_custom_command(
  OUTPUT ${STILLS_FIXTURE_PATHS}
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generate.sh ${FFMPEG_EXECUTABLE} ${STILLS_FIXTURE_DIR}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generate.sh
  COMMENT "Generating deterministic test clips with ffmpeg"
  VERBATIM)
add_custom_target(stills_fixtures DEPENDS ${STILLS_FIXTURE_PATHS})

# ---- unit / integration tests ---------------------------------------------------------------
add_executable(stills_tests
  test_time.cpp
  test_open.cpp
  test_sync.cpp
  test_containers.cpp
  test_keyframe.cpp
  test_output.cpp
  test_transform.cpp
  test_image.cpp
  test_options.cpp
  test_async.cpp
  test_cancel_io.cpp
  test_errors.cpp
  test_hw.cpp
  test_determinism.cpp
  test_realtime.cpp
  test_interactive.cpp
  test_coverage.cpp
  test_sweep.cpp
  test_soak.cpp
  test_positioning.cpp
  test_fixtures.cpp)
target_link_libraries(stills_tests PRIVATE stills::stills Catch2::Catch2WithMain)
target_include_directories(stills_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(stills_tests PRIVATE
  STILLS_FIXTURE_DIR="${STILLS_FIXTURE_DIR}"
  STILLS_HAVE_VP9=${STILLS_HAVE_VP9}
  STILLS_HAVE_X265=${STILLS_HAVE_X265})
stills_apply_warnings(stills_tests)
add_dependencies(stills_tests stills_fixtures)
catch_discover_tests(stills_tests TEST_PREFIX "stills." DISCOVERY_MODE PRE_TEST)

# ---- header-only check: three translation units link together --------------------------------
# The claim "header-only" is only true if every namespace-scope entity the headers define survives
# being defined in more than one TU. Two TUs that each drive the library, plus a main that calls
# both, is the smallest thing that can catch a missing `inline`.
add_executable(stills_odr odr/odr_a.cpp odr/odr_b.cpp odr/odr_main.cpp)
target_link_libraries(stills_odr PRIVATE stills::stills)
target_compile_definitions(stills_odr PRIVATE STILLS_FIXTURE_DIR="${STILLS_FIXTURE_DIR}")
stills_apply_warnings(stills_odr)
add_dependencies(stills_odr stills_fixtures)
add_test(NAME stills.odr COMMAND stills_odr)

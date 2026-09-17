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
  test_keyframe_index.cpp
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

# ---- build-time check: the value headers compile without FFmpeg -------------------------------
# Seven headers must reach no libav header so they can appear in a consumer's own public headers
# (README, "Design decisions"). This target deliberately does *not* link stills::stills: that would
# bring PkgConfig::FFMPEG's include directories and libraries with it, which is the property under
# test. The include directory is given directly, and SYSTEM, so the seven are seen exactly as every
# other target sees them. No add_test: there is nothing to check at runtime. See the TU's comment
# for why withholding the include directory is not by itself sufficient here.
add_library(stills_value_headers OBJECT value_headers/value_headers.cpp)
target_include_directories(stills_value_headers SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(stills_value_headers PRIVATE cxx_std_23)
stills_apply_warnings(stills_value_headers)

# The target above proves the seven compile standalone; it does not prove they reached no libav
# header. The #error in the TU is a five-macro blocklist and cannot be completed: about fifty public
# libav headers define none of the five, libavutil/display.h among them -- which is exactly the one
# stills_Geometry.h would reach for if anyone wanted av_display_rotation_get here. Verified by adding
# that include to stills_Geometry.h: stills_value_headers compiled green. So the property is tested here
# instead, against the dependency list the preprocessor produced, which no macro can hide from and
# which does not depend on the include directory having been withheld. The #error stays as the cheap
# first line: no second compiler invocation, and it names the offending file in the diagnostic.
# GCC/Clang only, matching stills_warnings.cmake: detail/stills_Config.h hard-errors without __int128,
# so an MSVC branch here could never run.
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  set(STILLS_VALUE_HEADER_DEPS ${CMAKE_CURRENT_BINARY_DIR}/value_headers.deps)
  add_custom_command(
    OUTPUT ${STILLS_VALUE_HEADER_DEPS}
    # -M, not -MM: -MM drops headers found in system directories, and libav's prefix is one of those
    # on this machine -- the very case this check exists for. Same -std as the target above, because
    # stills_Error.h and stills_Time.h select their <format> include on the language level.
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -I${PROJECT_SOURCE_DIR}/include -M
            -MF ${STILLS_VALUE_HEADER_DEPS}
            ${CMAKE_CURRENT_SOURCE_DIR}/value_headers/value_headers.cpp
    COMMAND ${CMAKE_COMMAND} -DDEPFILE=${STILLS_VALUE_HEADER_DEPS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/value_headers/assert_no_ffmpeg.cmake
    # The DEPENDS list is the FFmpeg-free closure itself -- the seven, plus the one detail header
    # all of them reach -- so editing any of them re-runs the check. Keep it in step with the
    # includes in value_headers.cpp.
    DEPENDS value_headers/value_headers.cpp
            value_headers/assert_no_ffmpeg.cmake
            ${PROJECT_SOURCE_DIR}/include/stills/stills_AssetInfo.h
            ${PROJECT_SOURCE_DIR}/include/stills/stills_Error.h
            ${PROJECT_SOURCE_DIR}/include/stills/stills_Geometry.h
            ${PROJECT_SOURCE_DIR}/include/stills/stills_Options.h
            ${PROJECT_SOURCE_DIR}/include/stills/stills_PixelFormat.h
            ${PROJECT_SOURCE_DIR}/include/stills/stills_Time.h
            ${PROJECT_SOURCE_DIR}/include/stills/stills_Version.h
            ${PROJECT_SOURCE_DIR}/include/stills/detail/stills_Config.h
    COMMENT "Checking that the value headers reach no libav header"
    VERBATIM)
  add_custom_target(stills_value_headers_check ALL DEPENDS ${STILLS_VALUE_HEADER_DEPS})
endif()

# ---- benchmark ---------------------------------------------------------------------------------
# Before/after numbers for the pipeline restructure: milliseconds per request, and the seek and
# decoded-frame counts that say whether the restructure changed which seeks happen. Built by `all`
# so it cannot rot while the pipeline is split, but registered with no test -- it has no pass or
# fail, and the numbers only mean something next to another revision's. Run it by hand on the
# release preset; see tests/bench/bench.cpp.
add_executable(stills_bench bench/bench.cpp)
target_link_libraries(stills_bench PRIVATE stills::stills)
target_compile_definitions(stills_bench PRIVATE STILLS_FIXTURE_DIR="${STILLS_FIXTURE_DIR}")
stills_apply_warnings(stills_bench)
add_dependencies(stills_bench stills_fixtures)

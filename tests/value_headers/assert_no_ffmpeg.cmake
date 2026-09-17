cmake_minimum_required(VERSION 3.25)
# tests/value_headers/assert_no_ffmpeg.cmake — run in script mode: cmake -DDEPFILE=<path> -P this.
#
# Fails the build when the dependency list of value_headers.cpp names a libav header. This is the
# FFmpeg-free property itself: it reads what the preprocessor actually opened, so unlike the #error
# in that TU it cannot be evaded by a header that defines none of the macros the #error knows, and
# it holds on a machine where withholding the include directory withholds nothing.
#
# The pattern matches any path with a libav*/ or libsw*/ directory component, which is every libav
# library there has ever been. Matching the directory rather than the file is what keeps this from
# being another blocklist.
file(READ "${DEPFILE}" STILLS_DEPS)
string(REGEX MATCHALL "[A-Za-z0-9_./+-]*lib(av|sw)[a-z]+/[A-Za-z0-9_]+\\.h" STILLS_LIBAV_HEADERS
       "${STILLS_DEPS}")
if(STILLS_LIBAV_HEADERS)
  list(REMOVE_DUPLICATES STILLS_LIBAV_HEADERS)
  list(JOIN STILLS_LIBAV_HEADERS "\n    " STILLS_LIBAV_LIST)
  # Removed so a generator that decides staleness from timestamps alone re-runs the check rather
  # than seeing a fresh output file left behind by a failed run.
  file(REMOVE "${DEPFILE}")
  message(FATAL_ERROR
    "a value header reached a libav header:\n    ${STILLS_LIBAV_LIST}\n"
    "stills_AssetInfo/stills_Error/stills_Geometry/stills_Options/stills_PixelFormat/stills_Time/stills_Version must depend on "
    "detail/stills_FFmpeg.h through nothing. See README, 'Design decisions'.")
endif()

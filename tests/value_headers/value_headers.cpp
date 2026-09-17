// tests/value_headers/value_headers.cpp — build-time proof that the value headers need no FFmpeg.
//
// Seven of the public headers reach no libav header, transitively, and the README promises it:
// "detail/ffmpeg.hpp is the single libav include point, so the value headers stay FFmpeg-free and
// can appear in a consumer's own public headers." Nothing tested it — every other TU in the suite
// includes stills/stills.hpp, which pulls in everything. This one is built as a target that neither
// links PkgConfig::FFMPEG nor names an FFmpeg include directory, so on a machine where FFmpeg lives
// in its own prefix an added include fails here and nowhere else.
//
// That structural half is not enough on every machine, and is not enough on this one. Where
// pkg-config names a directory the compiler already searches implicitly (-I/usr/local/include
// here), withholding the include directory withholds nothing: libav's headers are found anyway, and
// the guard would pass while the property was broken. Hence the two checks that follow it.
//
// The #error below is the cheap one, and it is a blocklist, not the property. It catches the five
// macros that come with avutil.h, avcodec.h, avformat.h and swscale.h -- the four a value header
// would most likely be made to reach -- and nothing else. About fifty public libav headers define
// none of the five: libavutil/display.h, rational.h, mathematics.h, error.h, buffer.h and dict.h
// among them. display.h is not a hypothetical: it is where av_display_rotation_get lives, which is
// the one thing geometry.hpp would ever want from libav. Adding <libavutil/display.h> to
// geometry.hpp compiles this TU green.
//
// The property itself is checked in tests/CMakeLists.txt, against the dependency list this TU's
// preprocessor produces: every header actually opened, whatever macros it defines and wherever it
// was found. That is the check to trust; this one is here because it costs nothing and names the
// file in the diagnostic.
//
// There is nothing to assert at runtime, so this is not registered as a test: it fails at build
// time or not at all. It exists mainly to survive the filename convention change, which rewrites
// every include in the tree and is the step most likely to reconnect one of these to FFmpeg.

#include <stills/asset_info.hpp>
#include <stills/error.hpp>
#include <stills/geometry.hpp>
#include <stills/options.hpp>
#include <stills/pixel_format.hpp>
#include <stills/time.hpp>
#include <stills/version.hpp>

#if defined(LIBAVFORMAT_VERSION_MAJOR) || defined(LIBAVCODEC_VERSION_MAJOR) || \
    defined(LIBAVUTIL_VERSION_MAJOR) || defined(LIBSWSCALE_VERSION_MAJOR) ||   \
    defined(AV_NOPTS_VALUE)
#error \
    "a value header reached a libav header: asset_info/error/geometry/options/pixel_format/time/version must depend on detail/ffmpeg.hpp through nothing. See README, 'Design decisions'."
#endif

namespace {

// Not behaviour assertions — the suite covers behaviour. Just enough of each header to make this a
// real compile of what the seven declare, rather than a compile of their includes.
static_assert(stills::Time{1, 2} == stills::Time{500, 1000});
static_assert(stills::Tolerance::exact().is_exact());
static_assert(stills::Size{4, 0}.is_empty());
static_assert(stills::Size{4, 2}.transposed() == stills::Size{2, 4});
static_assert(stills::to_string(stills::PixelFormat::rgba) == "rgba");
static_assert(stills::to_string(stills::HardwarePolicy::automatic) == "automatic");
static_assert(stills::to_string(stills::ErrorCode::decode_failed) == "decode_failed");
static_assert(stills::version == "0.1.0");

// AssetInfo and Options are not literal types (std::string members), so they are exercised here
// rather than in a static_assert.
[[maybe_unused]] bool value_headers_compose() {
  const stills::AssetInfo info;
  const stills::Options options;
  const stills::RequestOptions request;
  return !info.time_range().has_value() && options.validate().has_value() &&
         request.validate(options.pixel_format).has_value();
}

}  // namespace

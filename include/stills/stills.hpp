#pragma once
// stills — a header-only C++23 AVAssetImageGenerator over FFmpeg. Umbrella header.
//
// clang-format off
//   auto gen = stills::AssetImageGenerator::open("clip.mp4", {.maximum_size = stills::Size{320, 0}});
//   auto img = gen->image_at(1500ms);
// clang-format on
//
// <stills/interop.hpp> (raw libav access) is deliberately NOT included here.

#include "stills/asset_info.hpp"
#include "stills/async.hpp"
#include "stills/error.hpp"
#include "stills/generator.hpp"
#include "stills/geometry.hpp"
#include "stills/image.hpp"
#include "stills/log.hpp"
#include "stills/options.hpp"
#include "stills/pixel_format.hpp"
#include "stills/time.hpp"
#include "stills/version.hpp"

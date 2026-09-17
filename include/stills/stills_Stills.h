#pragma once
// stills — a header-only C++23 AVAssetImageGenerator over FFmpeg. Umbrella header.
//
//   auto gen = stills::AssetImageGenerator::open ("clip.mp4", { .maximumSize = stills::Size{ 320, 0 } });
//   auto img = gen->imageAt (1500ms);
//
// <stills/stills_Interop.h> (raw libav access) is deliberately NOT included here.

#include "stills/stills_AssetImageGenerator.h"
#include "stills/stills_AssetInfo.h"
#include "stills/stills_Async.h"
#include "stills/stills_Error.h"
#include "stills/stills_Geometry.h"
#include "stills/stills_Image.h"
#include "stills/stills_Log.h"
#include "stills/stills_Options.h"
#include "stills/stills_PixelFormat.h"
#include "stills/stills_Time.h"
#include "stills/stills_Version.h"

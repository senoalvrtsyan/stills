# stills

**A header-only C++23 `AVAssetImageGenerator` over FFmpeg.**

`0.1.0` · MIT · developed on Linux against FFmpeg 6.1, suite also run against 7.1.2, 8.0 and
9.0.1, with hardware decode exercised on VAAPI only · no CI.

Extracts still frames from a video asset at requested times, synchronously or asynchronously with
cancellation, modelled on Apple's `AVAssetImageGenerator`. Header-only C++23 over `libavformat` /
`libavcodec` / `libavutil` / `libswscale`: frame-accurate seeking by default with an optional
nearest-keyframe mode, aspect-preserving sizing, caller-chosen pixel formats, hardware decoding with
transparent software fallback, `std::expected` errors throughout.

It is not a player, transcoder or compositor: no audio, no presentation clock, no filter graph, no
encoder. One call asks for one time and gets one image back; everything else here exists to make
that answer correct on containers that make it hard.

---

**Contents**

- [Usage](#usage)
- [API index](#api-index)
- [Build and test](#build-and-test)
- [What it guarantees](#what-it-guarantees)
- [Errors](#errors)
- [Design decisions](#design-decisions)
- [Deviations from Apple](#deviations-from-apples-defaults)
- [Known limitations](#known-limitations)
- [License](#license)

---

## Usage

One frame, synchronously:

```cpp
#include <stills/stills.hpp>
using namespace std::chrono_literals;

auto gen = stills::AssetImageGenerator::open("clip.mp4");   // expected<AssetImageGenerator, Error>
if (!gen) return fail(gen.error());

auto img = gen->image_at(1500ms);  // or Time::seconds(1.5), or Time::frames(45, {30, 1})
if (!img) return fail(img.error());

std::cout << img->size() << " at " << img->actual_time() << '\n';
write_png(img->pixels(), img->row_stride(0), img->size());
```

A size box and a pixel format. `maximum_size` is a box to fit *within*: the aspect ratio is
preserved and sources are never upscaled, and a zero dimension is unconstrained — `Size{320, 0}` is
"at most 320 wide, any height". `Size{}`, both dimensions zero, is the native size.

```cpp
auto gen = stills::AssetImageGenerator::open(
    "clip.mp4", {.maximum_size = stills::Size{320, 0}, .pixel_format = stills::PixelFormat::rgb24});
```

Everything else defaults to: `PixelFormat::rgba`, `Tolerance::exact()` (frame-accurate), native
size, upright orientation and square pixels (the display matrix and the sample aspect ratio are both
applied), `Scaler::bicubic`, `OutOfRangePolicy::error`, and `HardwarePolicy::automatic`.

Hardware decode — and how to find out what you actually got:

```cpp
auto gen = stills::AssetImageGenerator::open(
    "clip.mp4", {.hardware = {.policy = stills::HardwarePolicy::prefer_hardware}});
//  software_only     never touches a device
//  automatic         the default; conservative — see "What it guarantees" before relying on it
//  prefer_hardware   always try, fall back to software silently
//  require_hardware  fail open() with hardware_unavailable rather than falling back
//  .device_type = stills::HardwareDeviceType::vaapi   pin one family (videotoolbox, cuda, qsv, …)

const auto d = gen->active_decoder();
std::cout << d.decoder_name << (d.hardware ? " on " + d.device_type_name : " (software)")
          << (d.fallback_reason.empty() ? "" : " — " + d.fallback_reason) << '\n';
```

A batch — one `Completion` per time, in request order, on the generator's worker thread:

```cpp
auto request = gen->generate_images({0s, 1s, 2s, 3s}, [&](stills::Completion c) {
  switch (c.status()) {  // c.index says which of the four times this is
    case stills::GenerationStatus::succeeded: use(c.index, std::move(*c.result)); break;
    case stills::GenerationStatus::failed:    log(c.requested_time, c.result.error()); break;
    case stills::GenerationStatus::cancelled: break;  // nothing decoded; the completion still came
  }
});
request.wait();
```

Cancellation at three scopes; every cancelled item still receives exactly one completion:

```cpp
auto request = gen->generate_images(times, std::move(handler));   // CompletionHandler is move-only
if (request.wait_for(2s) != stills::WaitResult::finished) request.cancel();  // this batch
request.wait();

gen->cancel_all();  // every queued and in-flight item of this generator

auto first_only = gen->generate_images(times, [](stills::Completion c) {
  if (c.index == 0) c.cancel_batch();  // from inside the handler, which has no handle yet
});
first_only.wait();
```

Nearest keyframe — the fast mode — is a tolerance, not a separate call, and applies to one call:

```cpp
auto key = gen->image_at(1500ms, {.tolerance = stills::Tolerance::any()});
```

Errors: nothing throws; `Error` carries an `ErrorCode`, the libav `AVERROR` and a message.

```cpp
auto gen = stills::AssetImageGenerator::open(
    path, {.out_of_range = stills::OutOfRangePolicy::clamp_to_last_frame});
if (!gen) {
  if (gen.error().code == stills::ErrorCode::no_video_stream) return skip(path);
  std::cerr << gen.error() << '\n';  // "file_not_found: avformat_open_input ... [AVERROR -2]"
  return 1;
}
auto last = gen->image_at(past_the_end);  // would be time_out_of_range by default
if (last) assert(last->adjustment() == stills::Adjustment::clamped_to_last);
```

`examples/thumbnail.cpp` (one frame to a PPM) and `examples/contact_sheet.cpp` (a batch tiled into a
sheet, Ctrl-C cancels) are complete programs in the same style.

## API index

Everything in namespace `stills`. `<stills/stills.hpp>` is the umbrella;
`<stills/interop.hpp>` is a separate opt-in for consumers who want the raw `AVFrame`.

| Type | Header | What it is |
|---|---|---|
| `AssetImageGenerator` | `generator.hpp` | The entry point. `open()` a source, then `image_at()` or `generate_images()`. Move-only; one decoder and one worker thread each. |
| `Image` | `image.hpp` | An owning, move-only still frame: planes as `std::span` with their strides, plus `actual_time()`, `is_keyframe()`, `adjustment()`, `is_corrupt()`. |
| `AsyncRequest` | `async.hpp` | Handle on one `generate_images()` batch — `cancel()`, `wait()`, `completed()`. Copies share the batch; dropping one does not cancel. |
| `Completion` | `async.hpp` | One delivered item: its `index`, `requested_time`, and `std::expected<Image, Error>`. `cancel_batch()` stops the rest from inside the handler. |
| `Time` | `time.hpp` | Rational media time (`value / timescale`) with invalid and infinite states. Implicit from integral `std::chrono` durations, so `1500ms` is a `Time`. |
| `Tolerance` | `time.hpp` | How far from the request a returned frame may be. `exact()` is the default; `any()` is nearest-keyframe, the fast mode. |
| `Size` | `geometry.hpp` | Width and height in pixels. |
| `Options` | `options.hpp` | Everything fixed at `open()`: output box, pixel format, tolerance, scaler, orientation, hardware policy, demuxer options, stream selection. |
| `RequestOptions` | `options.hpp` | The two that may vary per call — tolerance and output box. They travel with the request, so no lock is needed. |
| `Error`, `ErrorCode` | `error.hpp` | A failure: the code, the libav `AVERROR`, a message. Returned in `std::expected`, never thrown. |
| `AssetInfo` | `asset_info.hpp` | What `open()` learned: coded/display/output size, duration, codec, frame rate, rotation, colour metadata, seekability. |
| `PixelFormat` | `pixel_format.hpp` | Output layout — `rgba` by default, thirteen in all — with `plane_count()`, `bytes_per_pixel()` and the other layout helpers. |

Each field is documented where it is declared; `options.hpp` in particular carries the full
reference for every knob, including when you would want it.

## Build and test

| | |
|---|---|
| Compiler | C++23 with `std::expected` and `__int128`: GCC 13+, Clang 17+ with libc++ 16+, Xcode 15+. **MSVC is not supported.** |
| FFmpeg | 6.1 or newer (`static_assert`ed), found through `pkg-config`. Built and the full suite run here against **6.1** (libavformat 60), **7.1.2** (61), **8.0** (62) and **9.0.1** (63) — 121/121 on each, hardware decode included. One recovery path — clearing libavio's sticky error state after a cancelled read — pokes `AVIOContext` fields directly and is enabled only for those four majors; on any other it is skipped and a rewindable source is re-opened instead, costing one extra re-open per interrupt recovery rather than failing the build. On a non-rewindable source (a pipe) there is nothing to re-open and the interrupted read still fails — the same outcome as before, minus the build failure. |
| Build | CMake ≥ 3.25 and `Threads`. **Every preset specifies the Ninja generator**, so install `ninja` or configure by hand with `-G`. Catch2 v3.16.0 is fetched at configure time unless `find_package(Catch2 3)` finds one. |
| Standard | A top-level build pins `CMAKE_CXX_STANDARD 23`, `..._REQUIRED ON`, `..._EXTENSIONS OFF`, and sets `CXX_STANDARD 23` on the fetched Catch2 targets. CMake otherwise leaves Catch2 (`cxx_std_14`) at the compiler's default, and a standard mismatch makes our C++23 test TUs reference `StringMaker` specialisations Catch2 never emitted — the reported Apple Clang 17 link failure. Reproduced here only at C++14-vs-C++23; at C++17-vs-C++23 the symbol surface is identical on libstdc++, so **the Apple Clang mechanism itself is unverified — there is no Apple toolchain on this machine.** A consumer's settings are untouched. |
| Fixtures | An `ffmpeg` **binary** (6.0+, for `-display_rotation` and `-fps_mode`) with `libx264`, and `python3` for one clip — which falls back to `tr` when absent. `libvpx-vp9` and `libx265` are optional; their tests skip themselves. |

```sh
# Debian/Ubuntu — libav* 6.1 or newer, plus an ffmpeg binary for the test fixtures
sudo apt install pkg-config ninja-build ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
# macOS
brew install pkg-config ninja ffmpeg

cmake --preset default && cmake --build --preset default && ctest --preset default
```

> **If a system Catch2 is already installed.** `FIND_PACKAGE_ARGS 3` lets `find_package(Catch2 3)`
> win over the fetch, and a found Catch2 arrives as an *imported* target — `CXX_STANDARD` on an
> imported target does nothing, so a Homebrew Catch2 built at some other standard can reproduce
> the very link failure the pinned standard is there to prevent. Configure with
> `-DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON` to force the fetched copy, which is pinned.

Four presets: `default` (Debug — `-O0`, `assert()` live, so the documented precondition asserts
fire; time it with `release` instead), `release`, `asan` (Address + UndefinedBehaviour + Leak
sanitizers, with `tests/lsan.supp`) and `tsan` (ThreadSanitizer with `tests/tsan.supp` for
libavcodec's own thread pool; `stills.hw` is excluded because TSan aborts inside the NVIDIA driver).

> **Two platform caveats for the sanitizer presets.** On recent Linux kernels TSan needs the
> reduced ASLR range or *every* test dies with `unexpected memory mapping`:
> `setarch $(uname -m) -R ctest --preset tsan`. And the `asan` test preset sets
> `ASAN_OPTIONS=detect_leaks=1`, which is the default on Linux but is not supported on every
> Darwin target — drop that one variable if the preset refuses to start on macOS.

`ctest` runs 121 tests: 120 Catch2 cases plus one ODR executable of three translation units, which
is what makes "header-only" a checked claim rather than a hopeful one. The TSan suppression file is
deliberately broad, so the check on it is to run the `[async]` and `[cancel]` cases with it removed
entirely — they report nothing either way.

Warnings are errors for the library's own targets (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wold-style-cast …`; `-DSTILLS_WERROR=OFF` relaxes it), and nothing is imposed on
consumers: `stills::stills` carries only the include directory, C++23 and the link dependencies.

**Using it in your project.** Three routes to the same target:

```cmake
add_subdirectory(third_party/stills)   # vendored, or FetchContent_Declare(stills GIT_REPOSITORY …)
find_package(stills 0.1 REQUIRED)      # after `cmake --install`
target_link_libraries(app PRIVATE stills::stills)
```

Or skip CMake: put `include/` on the include path and link `libavformat libavcodec libavutil
libswscale` yourself. `STILLS_BUILD_TESTS`, `STILLS_BUILD_EXAMPLES` and `STILLS_INSTALL` default to
off for a consumer and on for a top-level build.

**Test fixtures.** `tests/fixtures/generate.sh <ffmpeg-binary> <output-dir>` regenerates every clip
the suite reads, and the build runs it automatically. The primary clip is 64×48 @ 30 fps, 4 s,
keyframes forced at 0/30/60/90, B-frames on, High profile 4:2:0, and **each frame's luma encodes its
own index**: left half `Y = 22 + 14·(N mod 16)`, right half `Y = 22 + 14·(N div 16)`. The tests
recover `N` through any decoder in any output format and compare it with the requested time, so the
assertion is a formula rather than a file hash — deterministic across FFmpeg builds and hardware.
The script also builds the awkward cases: MPEG-TS with a start offset, fragmented MP4,
Matroska/WebM, open-GOP, 10-bit, interlaced, raw elementary streams, rotations and mirrors,
anamorphic and VFR content, edit lists, and the corrupt, truncated and audio-only files the error
paths need.

## What it guarantees

- **Time origin.** `Time::zero()` is the asset's first *presented* frame whatever the container
  timestamps say — an MPEG-TS starting at 10 s is addressed from 0, an edit list that trims the
  opening frames moves zero to the first frame shown. `Image::actual_time()` is exact in the
  stream's time base: on Matroska, frame 29 of a 30 fps clip reports 967/1000, not 29/30.
- **Frame accuracy.** With `Tolerance::exact()` the frame returned is the one on screen at the
  requested time — the last frame at or before it, not the nearest by timestamp. Requests round to
  the *nearest* stream tick, so `Time::frames(k, fps)` returns frame `k` even where the time base
  cannot represent `k/fps` exactly.
- **Bounds.** Negative or non-finite times are `invalid_argument`. Past the last frame is
  `time_out_of_range`, or that frame flagged under `clamp_to_last_frame`; before the first presented
  frame the request clamps to it and is flagged. Requests more than one frame beyond the declared
  duration are refused without decoding, so a container that understates its duration still serves
  its last frame.
- **Tolerances.** `Tolerance::any()` returns the keyframe at or before the time and is the fastest
  mode — when the frame already held is that keyframe, nothing is seeked or decoded. A finite
  `before` stops the decode at the first frame inside the window; a finite `after` matters only when
  no frame at or before the time exists. Without a usable seek, `any()` falls back to the exact
  frame.
- **Sizing.** The box applies to the *rotated* image, aspect ratio is preserved, sources are never
  upscaled, anamorphic content is squared by default (the container's SAR declaration wins over the
  bitstream's, as in ffmpeg), and chroma-subsampled output rounds down to even dimensions.
- **Colour.** RGB and gray outputs are full range; YUV keeps the source range, and an RGB source
  converted to YUV is written limited-range (the ffmpeg convention), which `Image::color_range()`
  reports. HDR transfer functions are **not** mapped: PQ/HLG thumbnails come out flat, and
  `AssetInfo::color_transfer` says what the compositor is looking at.
- **Corruption.** Damaged packets are skipped; frames the decoder conceals, and every frame after a
  decode error until the next keyframe, come back with `is_corrupt() == true` rather than silently.
  A request with nothing decodable fails, and the generator stays usable.
- **Untrusted input.** The source string reaches every protocol this FFmpeg build has, so a
  path-prefix check proves nothing — `concat:`, `subfile,` and playlists all open. Confine it with
  libavformat's own keys, which apply to nested opens too:
  `demuxer_options = {{"protocol_whitelist", "file"}, {"format_whitelist", "mov,mp4,matroska"}}`,
  plus `max_input_pixels`. That does not make a decoder safe against a malicious *bitstream*; it
  removes the ways a *name* or a header *field* alone can reach other files or exhaust memory.
- **Hardware.** `automatic` (the default) uses hardware for HEVC/AV1/VP9/VVC at 720p and above, and
  software for H.264 at every size. **Read the next bullet before relying on that**, because the
  rule is more conservative than the hardware on your machine probably warrants.
- **Which decoder is actually faster.** Measured here — Intel Iris Xe via VAAPI and an RTX A2000 via
  NVDEC, against a 20-thread i7-12700H, so software gets 16 frame threads and a generous baseline.
  40 requests, `ms` per returned frame:

  | random-access (what this library does) | software | VAAPI | NVDEC |
  |---|---|---|---|
  | H.264 854×480 | 8.3 | **5.2** | — |
  | H.264 1280×720 | 18.2 | **9.6** | — |
  | H.264 1920×1080 | 29.4–31.1 | **19.8–20.2** | 137.0 |
  | H.264 3840×2160 | 97.1 | **75.0** | 220.6 |
  | HEVC 3840×2160 | 199.1 | 47.2 | **41.2** |

  Two things fall out. Hardware wins H.264 at *every* size on VAAPI — so the codec rule above is
  wrong for that family, and `prefer_hardware` is worth 1.3–1.9× on it. And NVDEC H.264 is 4.7×
  *slower* than software, because the H.264 decoder renegotiates its pixel format on every
  `avcodec_flush_buffers()` — one per seek — and NVDEC rebuilds the decoder each time, while HEVC on
  NVDEC does not renegotiate and is fine. That single driver pathology is where the codec rule came
  from; generalising it to every family is the part that does not hold.

  It is kept as the default anyway, deliberately. Always-hardware would make that NVDEC case the
  worst case (4.7× slower than doing nothing); always-software caps the loss at the ~1.9× above.
  The honest fix is to put the exception on the *device family* rather than the codec, and that is a
  behaviour change worth more than one machine's timings — which is exactly the mistake the current
  rule already made. So: treat `automatic` as a floor, not as advice. **If your input is H.264 and
  your device is VAAPI, VideoToolbox or QSV, set `prefer_hardware` and measure.** Note also that
  hardware is the wrong choice for *sequential* access at any codec — software is 2–3× faster there,
  because the per-frame GPU readback stops being amortised.

  Any policy that tries hardware validates it by decoding the first frame inside `open()`, so a
  device that cannot be created or a profile the decoder declines is resolved before `open()`
  returns. It is not permanent, though: a hardware fault *during* decoding rebuilds the decoder once
  and then falls back to software, so `active_decoder()` is a snapshot rather than a constant, and
  its `fallback_reason` says why the current path was chosen.

## Errors

`std::expected<T, Error>` everywhere, one failure channel for both modes, `Error{code, av_error,
message}`. `to_string`, `operator<<` and a `std::formatter` are provided; `error_category()` and
`make_error_code` bridge `ErrorCode` to `std::error_code`.

| `ErrorCode` | When you get it |
|---|---|
| `file_not_found`, `open_failed` | the source does not exist; `avformat_open_input` failed otherwise — permissions, protocol, a refused connection |
| `unsupported_format`, `no_video_stream` | not a media container, stream info unreadable, or `max_input_pixels` exceeded; audio-only asset, or only cover art |
| `decoder_not_found`, `decoder_open_failed`, `hardware_unavailable` | no decoder for the codec; `avcodec_open2` failed; `require_hardware` and no hardware path worked |
| `invalid_argument`, `invalid_state` | a bad `Options` combination, a negative or non-finite time, an unknown demuxer key, an empty source or one with an embedded NUL; a moved-from or `close()`d generator |
| `time_out_of_range`, `end_of_stream` | past the last frame under `OutOfRangePolicy::error`; the stream ended before a frame covering the request appeared |
| `decode_failed`, `conversion_failed`, `out_of_memory` | corrupt data; scaling, conversion or a hardware transfer failed; allocation failed — reported as a failed request rather than left to terminate the worker |
| `seek_failed`, `not_seekable`, `unusable` | positioning failed and re-opening failed; the source cannot be rewound and the request needs an earlier position; the input had to be re-opened and that failed — the next request retries |
| `cancelled`, `internal` | `cancel()`, `cancel_all()`, `cancel_batch()`, or the generator was destroyed; should-not-happen |

## Design decisions

```
stills::AssetImageGenerator (move-only handle) ──shared_ptr──▶ detail::Engine (heap, never moves)
   │ image_at(Time) ─── lock decoder_mutex ─────────────────▶ ├─ detail::FramePipeline (all libav state)
   │ generate_images(times, handler) ── enqueue ────────────▶ ├─ deque<shared_ptr<Batch>> + cv
   │ cancel_all()                                            ├─ std::thread worker
   └─ AsyncRequest (copyable) ──shared_ptr──▶ detail::Batch   └─ per item: lock; image_at(t, token);
                                                                 unlock; handler(Completion)
```

### Accurate seeking

`avformat_seek_file` lands where it likes — before the target, after it, or on a packet that is not
a keyframe at all — so rather than decode and hope, the pipeline reads the first keyframe *packet*
after the seek and compares it with the target, before anything is decoded. A landing past the
target then costs a cheap re-seek rather than a decoded-and-discarded GOP.

On a container with a trusted index a late landing re-seeks to `key.pts − 1 − delay`, up to three
rounds, and the reorder shift it learns is remembered so later seeks land right first time — which
is what fragmented MP4 needs, because the mov demuxer does not apply that shift to fragments. On
MPEG-TS there is no index, so the pipeline aims one learned GOP early, scans packets *without
decoding* for the covering keyframe, and builds an index of keyframe times, byte positions and GOP
extents as it goes; a later request into that GOP positions with one byte seek. Decoded frames are
the second line of evidence: a first frame past the target still triggers the back-off, and only
four narrow conditions may conclude that a request precedes the first frame — a clamp is never
guessed. Three consecutive seek failures switch the container to forward-decode or re-open, because
a wrong frame is worse than a slow one and a failed seek must never continue from an unknown
position.

### The decode loop

Pull before push: `avcodec_receive_frame` until `EAGAIN`, then `av_read_frame` →
`avcodec_send_packet`, and at end of file drain once. A frame's timestamp is
`best_effort_timestamp`, else `pts`, else `pkt_dts`, else synthesised from the previous frame.

For target `P` with tolerance window `[lo, hi]`: a frame before `P` becomes the *held* frame
(returned at once if it is already inside the window); a frame at `P` returns; a frame past `P`
returns the held frame and is itself kept as the look-ahead, which is what makes the next request
cost no I/O at all when `held ≤ P < pending`. At end of file the held frame is returned when `P`
falls inside its display interval — recomputed from the frame's own duration every time, never
remembered as a flag, because a stale "this is the last frame" belief is exactly how a second
request gets answered with the wrong frame.

Decoding forward beats seeking whenever the keyframe covering `P` has already been fed to the
decoder; that comparison is made in the DTS domain on both sides, never a DTS against a PTS. When a
keyframe lies ahead instead, learned per-seek and per-frame costs decide. Far from the target,
non-reference frames are skipped with `AVDISCARD_NONREF` — but only once a frame with a *later*
presentation time has been fed and that later time is itself before the window, which proves the
skipped frame is off screen whatever the container claims about durations. Durations cannot prove
it, because mov stores decode-order deltas: a frame held on screen for seconds on a VFR stream still
carries a 1/30 s duration. Every skipped or corrupt frame's time is recorded as a *hole*, and no
later request may conclude anything across one — which is what stops a request following a cancelled
one from being answered out of stale state.

### Time

A CMTime-like rational `Time{value, timescale, kind}` with FFmpeg-free arithmetic in `__int128`,
IEEE-like infinity rules and a `<=>` total order (`invalid < -inf < finite < +inf`). Conversion from
an integral `std::chrono::duration` with a `1/N` period is implicit because it is exact, so
`image_at(1500ms)` and `image_at(2min)` read naturally with no floating point on the path;
`Time::seconds(double)` is the explicit lossy route. Scaling by a `double` is deliberately **not** an
operator — `t * 1.5` would have truncated to `t * 1` in silence — so that overload is deleted and
the caller writes the rounding. `__int128` is why MSVC is out.

### Image ownership

An `Image` owns the converted `AVFrame` and nothing copies it on the way out. Pixels are exposed as
`std::span` plus `row_stride()` per plane, because decoder and scaler output is padded and
pretending otherwise is how a consumer gets a sheared picture; `copy_packed_to()` and
`to_packed_bytes()` are there for a caller that wants a packed buffer and knows it is paying for
one. Output buffers come from per-geometry `AVBufferPool`s and return to the pool when the `Image`
dies, so an image may safely outlive its generator. Provenance travels with the pixels:
`actual_time()`, `duration()`, `is_keyframe()`, `adjustment()`, `is_corrupt()`.

### Resource wrapping

One deleter template, `AvDeleter<Fn>`, dispatches on libav's two free-function shapes — `void(T**)`
and `void(T*)` — and yields `FormatCtxPtr`, `CodecCtxPtr`, `FramePtr`, `PacketPtr`, `BufferRefPtr`,
`SwsCtxPtr` and `DictPtr`, so there is one place in the library that knows how libav frees things.
Creation functions with a `T**` out-parameter are wrapped only *after* success, which closes by
construction the classic libav leak where a half-initialised context is dropped on an error path.
Frames are `av_frame_unref`'d each iteration and moved with `av_frame_move_ref`, never copied; the
codec context owns its `hw_device_ctx` reference and the pipeline owns the hardware device buffer
and the `get_format` state. `detail/ffmpeg.hpp` is the single libav include point, so the value
headers stay FFmpeg-free and can appear in a consumer's own public headers.

### Concurrency

The engine is heap-allocated and never moves — libav callbacks and the worker hold pointers into it
— and the generator is a move-only handle to a `shared_ptr` of it. One decoder and one worker thread
per generator; options are fixed at `open()` so the worker can never read a field the caller is
mutating, and the two that vary per call travel with the request in `RequestOptions`.

| Operation | Any thread | From inside a completion handler |
|---|---|---|
| `image_at` | yes — serialised on the decoder | yes |
| `generate_images`, `cancel_all`, `close`, `cancel`, `cancel_batch` | yes | yes |
| `wait` / `wait_for` | yes | returns `refused_on_worker_thread` at once — waiting there would deadlock |
| `info()`, `options()`, `active_decoder()` | yes | yes |
| destroy the generator | yes, not concurrently with other calls on the same object | yes — teardown completes on the worker |

The worker takes one *item* at a time from the front batch of a single FIFO queue, and a batch stays
at the front until its last item has been dispatched — so items of one batch are delivered in
request order while the queue stays FIFO by batch. Before each item it waits until no synchronous
caller is queued for the decoder, because `std::mutex` is unfair and an `image_at()` behind a long
batch would otherwise have no bound on how long it waits. That deference is capped at 20 ms: an
*unbounded* priority is the same trap pointing the other way, since threads calling `image_at()` in
a loop never leave the queue empty for the worker to observe.

An item either delivers `cancelled` without touching the decoder at all, or takes the decoder mutex,
decodes under a cancel token checked once per packet *and* wired into libavformat's interrupt
callback — so a cancellation reaches a thread blocked inside I/O, not only one between packets —
releases the mutex, and only then runs the handler, which is why a handler may call straight back
in. After a batch's last item the handler is destroyed on the worker *before* the completed count is
incremented, so nobody can see the batch finish while its captured state still exists. Teardown
cancels everything queued and joins the worker, or detaches it when the generator is destroyed from
inside a handler; the drain re-checks the queue under the lock, so a handler that chains one more
batch on the way down still gets its completions.

The contract, then: exactly one `Completion` per requested time, in request order, always on the
worker thread; a `failed` item does not stop its batch; undelivered items arrive as `cancelled`.
`AsyncRequest` copies share one batch, so `cancel()` on any of them cancels it for all, and dropping
a handle does not cancel. Handlers must not throw, as with any `std::thread`.

## Deviations from Apple's defaults

| | Apple | stills | Why |
|---|---|---|---|
| type name | `AVAssetImageGenerator` | `stills::AssetImageGenerator` | the `AV` prefix is the framework's namespace; `stills::` already does that job |
| default tolerance | infinite (nearest keyframe) | exact | frame accuracy is the core behaviour asked for; Apple's default is a well-known footgun |
| `appliesPreferredTrackTransform` | `false` | `true` | upright output is what nearly every consumer wants; turn it off for coded-orientation pixels |
| mutable properties | yes | fixed at `open()`, with `RequestOptions` per call | a worker thread reading options the caller mutates is a data race |
| `actualTime` | out-parameter | `Image::actual_time()` | there is no actual time to read when there is no image |
| `copyCGImage` | throws `NSError` | `std::expected<Image, Error>` | one error channel for the synchronous and asynchronous paths |
| past the end | clamps | `time_out_of_range`, clamping opt-in | surfaces caller bugs; `OutOfRangePolicy::clamp_to_last_frame` restores Apple's behaviour |
| `apertureMode`, `videoComposition` | `cleanAperture`; a composition may be applied | neither implemented | clean-aperture cropping is a QuickTime convention and compositing is a different library |

## Known limitations

- One decoder per generator: parallel extraction across far-apart times needs several generators; a
  decoder pool would slot in behind the same API. Options other than the tolerance and the output
  box cannot change after `open()` — the pixel format determines the converter.
- `HardwarePolicy::automatic` is a static table, not a measurement: it cannot know that this
  machine's GPU beats its CPU, or the reverse. The pipeline already learns seek and per-frame costs
  at runtime (`SeekCostModel`, feeding the seek-versus-decode-forward decision), and the NVDEC
  pathology above is observable from this library's own `get_format` callback — counting
  renegotiations per seek would detect it on any device instead of naming codecs. That is the
  intended 1.0 answer; until then, `prefer_hardware` plus `active_decoder()` is the honest override.
- `open()` blocks on all the I/O the demuxer does and cannot be cancelled; bound it with
  `demuxer_options` such as `rw_timeout`.
- Only right-angle display matrices are honoured (0/90/180/270 plus a horizontal mirror); other
  angles snap to the nearest right angle. 10-bit sources keep their depth only through `p010` and
  `rgba64`. No HDR tone mapping, no deinterlacing, no clean-aperture cropping.
- `Image::duration()` is the container's per-frame duration or the average interval; on VFR content
  only `actual_time()` is authoritative.
- Non-rewindable inputs (`pipe:`) support forward requests only; an earlier time fails with
  `not_seekable` and the generator stays usable. Hardware decode is never attempted on them, because
  validating a candidate decodes the first frame and a rejected candidate needs a rewind.
- Timestamp-less containers (raw H.264/HEVC elementary streams) are decoded forward and re-opened
  for backward requests; frames are stamped in display order from the codec frame rate — libav's 25
  fps default when the stream carries none, flagged by `AssetInfo::timestamps_synthesized`.
- Header-only costs: any TU including `image.hpp`, `generator.hpp` or the umbrella header also sees
  the libav headers and ~1,000 of their macros, 52 unprefixed (`MKTAG`, `M_PI`, `NAN`, …) — the
  value headers are FFmpeg-free so they can appear in yours instead. A plugin embedding stills also
  cannot be unloaded: GCC gives its function-local statics `STB_GNU_UNIQUE` binding and glibc marks
  such objects `NODELETE`, so build one with `-fno-gnu-unique` if it must `dlclose()`.
- Exceptions are used internally though none crosses the API, so `-fno-exceptions` is unsupported
  (`-fno-rtti` is fine). AV1, VVC, ProRes and DNxHR are expected to work but are untested here, as
  is every hardware family but VAAPI. There is no CI.

## License

MIT — see [LICENSE](LICENSE).

FFmpeg is a separate dependency under its own terms, and those terms follow how *your* FFmpeg was
configured rather than anything here: libav* is LGPL-2.1-or-later as normally built, but a build
configured `--enable-gpl` (x264, x265) is GPL, and `--enable-nonfree` is redistributable not at all.
stills links whatever `pkg-config` finds, so check that build before you ship one.

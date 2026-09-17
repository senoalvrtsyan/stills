# stills

**A header-only C++23 `AVAssetImageGenerator` over FFmpeg.**

`0.1.0` · MIT · developed on Linux against FFmpeg 6.1, suite also run against 7.1.2, 8.0 and
9.0.1, with hardware decode exercised on VAAPI and NVDEC here and VideoToolbox in review · no CI.

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
#include <stills/stills_Stills.h>
using namespace std::chrono_literals;

auto gen = stills::AssetImageGenerator::open ("clip.mp4"); // expected<AssetImageGenerator, Error>

if (! gen) return fail (gen.error());

auto img = gen->imageAt (1500ms); // or Time::seconds(1.5), or Time::frames(45, {30, 1})

if (! img) return fail (img.error());

std::cout << img->getSize() << " at " << img->getActualTime() << '\n';
writePng (img->getPixels(), img->getRowStride (0), img->getSize());
```

A size box and a pixel format. `maximumSize` is a box to fit *within*: the aspect ratio is
preserved and sources are never upscaled, and a zero dimension is unconstrained — `Size{320, 0}` is
"at most 320 wide, any height". `Size{}`, both dimensions zero, is the native size.

```cpp
auto gen = stills::AssetImageGenerator::open (
    "clip.mp4", { .maximumSize = stills::Size{ 320, 0 }, .pixelFormat = stills::PixelFormat::rgb24 });
```

Everything else defaults to: `PixelFormat::rgba`, `Tolerance::exact()` (frame-accurate), native
size, upright orientation and square pixels (the display matrix and the sample aspect ratio are both
applied), `Scaler::bicubic`, `OutOfRangePolicy::error`, and `HardwarePolicy::automatic`.

Hardware decode — and how to find out what you actually got:

```cpp
auto gen = stills::AssetImageGenerator::open ("clip.mp4",
                                              { .hardware = { .policy = stills::HardwarePolicy::preferHardware } });
//  softwareOnly     never touches a device
//  automatic         the default; conservative — see "What it guarantees" before relying on it
//  preferHardware   always try, fall back to software silently
//  requireHardware  fail open() with hardwareUnavailable rather than falling back
//  .deviceType = stills::HardwareDeviceType::vaapi   pin one family (videotoolbox, cuda, qsv, …)

const auto d = gen->getActiveDecoder();
std::cout << d.decoderName << (d.hardware ? " on " + d.deviceTypeName : " (software)")
          << (d.fallbackReason.empty() ? "" : " — " + d.fallbackReason) << '\n';
```

A batch — one `Completion` per time, in request order, on the generator's worker thread:

```cpp
auto onFrame = [&] (stills::Completion c)
{
    switch (c.getStatus())
    { // c.index says which of the four times this is
    case stills::GenerationStatus::succeeded:
        use (c.index, std::move (*c.result));
        break;
    case stills::GenerationStatus::failed:
        log (c.requestedTime, c.result.error());
        break;
    case stills::GenerationStatus::cancelled:
        break; // nothing decoded; the completion still came
    }
};

auto request = gen->generateImages ({ 0s, 1s, 2s, 3s }, std::move (onFrame));
request.wait();
```

Cancellation at three scopes; every cancelled item still receives exactly one completion:

```cpp
auto request = gen->generateImages (times, std::move (handler)); // CompletionHandler is move-only

if (request.waitFor (2s) != stills::WaitResult::finished) request.cancel(); // this batch
request.wait();

gen->cancelAll(); // every queued and in-flight item of this generator

auto stopAfterFirst = [] (stills::Completion c)
{
    // From inside the handler, which has no AsyncRequest handle yet.
    if (c.index == 0) c.cancelBatch();
};

auto firstOnly = gen->generateImages (times, std::move (stopAfterFirst));
firstOnly.wait();
```

Nearest keyframe — the fast mode — is a tolerance, not a separate call, and applies to one call:

```cpp
auto key = gen->imageAt (1500ms, { .tolerance = stills::Tolerance::any() });
```

Errors: nothing throws; `Error` carries an `ErrorCode`, the libav `AVERROR` and a message.

```cpp
auto gen = stills::AssetImageGenerator::open (path, { .outOfRange = stills::OutOfRangePolicy::clampToLastFrame });

if (! gen)
{
    if (gen.error().code == stills::ErrorCode::noVideoStream) return skip (path);
    std::cerr << gen.error() << '\n'; // "fileNotFound: avformat_open_input ... [AVERROR -2]"
    return 1;
}

auto last = gen->imageAt (pastTheEnd); // would be timeOutOfRange by default

if (last) assert (last->getAdjustment() == stills::Adjustment::clampedToLast);
```

`examples/thumbnail.cpp` (one frame to a PPM) and `examples/contact_sheet.cpp` (a batch tiled into a
sheet, Ctrl-C cancels) are complete programs in the same style.

## API index

Everything in namespace `stills`. `<stills/stills_Stills.h>` is the umbrella;
`<stills/stills_Interop.h>` is a separate opt-in for consumers who want the raw `AVFrame`.

| Type | Header | What it is |
|---|---|---|
| `AssetImageGenerator` | `stills_AssetImageGenerator.h` | The entry point. `open()` a source, then `imageAt()` or `generateImages()`. Move-only; one decoder and one worker thread each. |
| `Image` | `stills_Image.h` | An owning, move-only still frame: planes as `std::span` with their strides, plus `getActualTime()`, `isKeyframe()`, `getAdjustment()`, `isCorrupt()`. |
| `AsyncRequest` | `stills_Async.h` | Handle on one `generateImages()` batch — `cancel()`, `wait()`, `getCompleted()`. Copies share the batch; dropping one does not cancel. |
| `Completion` | `stills_Async.h` | One delivered item: its `index`, `requestedTime`, and `std::expected<Image, Error>`. `cancelBatch()` stops the rest from inside the handler. |
| `Time` | `stills_Time.h` | Rational media time (`value / timescale`) with invalid and infinite states. Implicit from integral `std::chrono` durations, so `1500ms` is a `Time`. |
| `Tolerance` | `stills_Time.h` | How far from the request a returned frame may be. `exact()` is the default; `any()` is nearest-keyframe, the fast mode. |
| `Size` | `stills_Geometry.h` | Width and height in pixels. |
| `Options` | `stills_Options.h` | Everything fixed at `open()`: output box, pixel format, tolerance, scaler, orientation, hardware policy, demuxer options, stream selection. |
| `RequestOptions` | `stills_Options.h` | The two that may vary per call — tolerance and output box. They travel with the request, so no lock is needed. |
| `Error`, `ErrorCode` | `stills_Error.h` | A failure: the code, the libav `AVERROR`, a message. Returned in `std::expected`, never thrown. |
| `AssetInfo` | `stills_AssetInfo.h` | What `open()` learned: coded/display/output size, duration, codec, frame rate, rotation, colour metadata, seekability. |
| `PixelFormat` | `stills_PixelFormat.h` | Output layout — `rgba` by default, thirteen in all — with `getPlaneCount()`, `getBytesPerPixel()` and the other layout helpers. |

Each field is documented where it is declared; `stills_Options.h` in particular carries the full
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
  opening frames moves zero to the first frame shown. `Image::getActualTime()` is exact in the
  stream's time base: on Matroska, frame 29 of a 30 fps clip reports 967/1000, not 29/30.
- **Frame accuracy.** With `Tolerance::exact()` the frame returned is the one on screen at the
  requested time — the last frame at or before it, not the nearest by timestamp. Requests round to
  the *nearest* stream tick, so `Time::frames(k, fps)` returns frame `k` even where the time base
  cannot represent `k/fps` exactly.
- **Bounds.** Negative or non-finite times are `invalidArgument`. Past the last frame is
  `timeOutOfRange`, or that frame flagged under `clampToLastFrame`; before the first presented
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
  converted to YUV is written limited-range (the ffmpeg convention), which `Image::getColorRange()`
  reports. HDR transfer functions are **not** mapped: PQ/HLG thumbnails come out flat, and
  `AssetInfo::colorTransfer` says what the compositor is looking at.
- **Corruption.** Damaged packets are skipped; frames the decoder conceals, and every frame after a
  decode error until the next keyframe, come back with `isCorrupt() == true` rather than silently.
  A request with nothing decodable fails, and the generator stays usable.
- **Untrusted input.** The source string reaches every protocol this FFmpeg build has, so a
  path-prefix check proves nothing — `concat:`, `subfile,` and playlists all open. Confine it with
  libavformat's own keys, which apply to nested opens too:
  `demuxerOptions = {{"protocol_whitelist", "file"}, {"format_whitelist", "mov,mp4,matroska"}}`,
  plus `maxInputPixels`. That does not make a decoder safe against a malicious *bitstream*; it
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
  wrong for that family, and `preferHardware` is worth 1.3–1.9× on it. And NVDEC H.264 is 4.7×
  *slower* than software, because the H.264 decoder renegotiates its pixel format on every
  `avcodec_flush_buffers()` — one per seek — and NVDEC rebuilds the decoder each time, while HEVC on
  NVDEC does not renegotiate and is fine. That single driver pathology is where the codec rule came
  from; generalising it to every family is the part that does not hold.

  A third machine, contributed by the review of this code: on Apple silicon, **VideoToolbox was
  slower than software on H.264**. That is the opposite direction from VAAPI on the same codec, and
  the cause is neither NVDEC's nor VAAPI's — on unified memory the CPU decoder is fast and the frame
  is already where the converter wants it, while the GPU path pays session setup per flush plus a
  surface download for a frame software hands over for free. Three device families, three different
  answers for one codec. No table keyed on the codec can be right on all three.

  It is kept as the default anyway, for now. Always-hardware would make the NVDEC case the worst
  case (4.7× slower than doing nothing); always-software caps the loss at the ~1.9× above. So:
  treat `automatic` as a floor, not as advice. **If your input is H.264 and your device is VAAPI or
  QSV, set `preferHardware` and measure; on VideoToolbox, measure before you do.** Note also that
  hardware is the wrong choice for *sequential* access at any codec — software is 2–3× faster there,
  because the per-frame GPU readback stops being amortised.

#### What `automatic` should be

Measured, not predicted — and the measurement is already being taken. `SeekCostModel` maintains two
exponential averages per generator: **seek cost** (positioning plus the first frame out of a flushed
decoder) and **per-frame cost** (each further frame), split at the moment the first frame arrives.
That split is what separates all three pathologies above: NVDEC's decoder rebuild and VideoToolbox's
session setup land in the seek cost, VideoToolbox's surface download lands in the per-frame cost.
Both are measured on your content, on your machine, and they already drive the
seek-versus-decode-forward decision.

Counting `get_format` renegotiations — the obvious instrumentation, and what an earlier draft of
this README proposed — is the wrong *primary* signal, not merely an incomplete one. A count is a
frequency, not a cost: H.264 renegotiates once per seek on *every* device, and the same count is a
cheap context re-init on VAAPI, where hardware still wins, and a full CUVID rebuild on NVDEC, where
it dominates — roughly ten times the cost, flagged identically. And VideoToolbox's problem is not
renegotiation at all, so a counter would never fire on the one machine whose answer it most needs to
change.

The decision is about *(codec, resolution class, device type)* — not about the file — so it is made
**once per process per such key**, not once per generator:

1. `open()`: if the process already holds a decision for this key, start on the winning path.
   Otherwise the codec/size table above is the *initial guess* — free, and right often enough to skip
   the measurement in the common case.
2. The first generator for a key runs that guess for about five requests, far enough past the cold
   first seek for the averages to settle.
3. Then **one** A/B on real content: decode the next request on the other path as well — same time,
   same content, same output size — and compare total ms. One extra decode, not a synthetic probe.
   An A/B inside `open()` is the wrong place: it doubles the already-slow hardware open, and a
   three-seek sample is noisy in exactly the close cases.
4. Keep the winner and cache it for the process. Switching reuses the decoder rebuild that already
   exists for hardware-fault fallback. Later generators with the same key skip steps 2–3 and start
   on the winning path. No re-decision within a process.
5. Report it: `getActiveDecoder().fallbackReason` carries the numbers — *"automatic: software
   9.6 ms/frame vs videotoolbox 14.1 ms/frame on this content"* — and the cached decision is
   readable, so an operator can copy it into deployment config.

On Apple silicon and H.264 the table says software, the first generator A/Bs VideoToolbox once, it
loses, and every later H.264 generator in that process starts on software — the same answer as
today, now because it was measured rather than guessed. On VAAPI the A/B tries hardware, hardware
wins, and later generators start there, which today they never would. On NVDEC the A/B tries it, it
is 4.7× slower, and it stays on software — the case the table was written for still comes out right.

The cost: one extra decode per distinct content class per process, plus about five requests on a
possibly wrong path for the first generator only. A process that opens one short file and exits
behaves like today's table plus one A/B. The cache is process-wide shared state, which this library
otherwise avoids; it needs a mutex and a reset hook, and that is the price. A renegotiation counter
still earns a place in this design — as a *diagnostic* on `ActiveDecoder`, because it is what
explains a bad hardware number to whoever reads the log. Not as the decision input.

**None of this is implemented.** This revision changes no hardware behaviour: the table above is
still exactly what `automatic` does. The argument is written down here and in
`VideoDecoder::isHardwareWorthwhile`'s contract comment so that the next change to it starts from a
reasoned position rather than from one more machine's timings.

#### When to measure and when to configure

Measuring per machine is the right default when nobody configured the machine: a desktop
application, or a service whose pool mixes CPU-only and GPU nodes where the code cannot know which
node it is running on. Inside a pool you control, the explicit policies beat any automatic one.
`preferHardware`, `softwareOnly` and `hardware.deviceType` in deployment config give deterministic
pixels across nodes — hardware and software decoders can differ at the pixel level, which quietly
breaks content-hash caching — no warm-up, and no per-process A/B. The readable cached decision is
how you find out what to put there. `automatic` is a default for unknown machines; a fleet is known
machines.

  Any policy that tries hardware validates it by decoding the first frame inside `open()`, so a
  device that cannot be created or a profile the decoder declines is resolved before `open()`
  returns. It is not permanent, though: a hardware fault *during* decoding rebuilds the decoder once
  and then falls back to software, so `getActiveDecoder()` is a snapshot rather than a constant, and
  its `fallbackReason` says why the current path was chosen.

## Errors

`std::expected<T, Error>` everywhere, one failure channel for both modes, `Error{code, avError,
message}`. `toString`, `operator<<` and a `std::formatter` are provided; `getErrorCategory()` and
`make_error_code` bridge `ErrorCode` to `std::error_code`.

| `ErrorCode` | When you get it |
|---|---|
| `fileNotFound`, `openFailed` | the source does not exist; `avformat_open_input` failed otherwise — permissions, protocol, a refused connection |
| `unsupportedFormat`, `noVideoStream` | not a media container, stream info unreadable, or `maxInputPixels` exceeded; audio-only asset, or only cover art |
| `decoderNotFound`, `decoderOpenFailed`, `hardwareUnavailable` | no decoder for the codec; `avcodec_open2` failed; `requireHardware` and no hardware path worked |
| `invalidArgument`, `invalidState` | a bad `Options` combination, a negative or non-finite time, an unknown demuxer key, an empty source or one with an embedded NUL; a moved-from or `close()`d generator |
| `timeOutOfRange`, `endOfStream` | past the last frame under `OutOfRangePolicy::error`; the stream ended before a frame covering the request appeared |
| `decodeFailed`, `conversionFailed`, `outOfMemory` | corrupt data; scaling, conversion or a hardware transfer failed; allocation failed — reported as a failed request rather than left to terminate the worker |
| `seekFailed`, `notSeekable`, `unusable` | positioning failed and re-opening failed; the source cannot be rewound and the request needs an earlier position; the input had to be re-opened and that failed — the next request retries |
| `cancelled`, `internal` | `cancel()`, `cancelAll()`, `cancelBatch()`, or the generator was destroyed; should-not-happen |

## Design decisions

```
stills::AssetImageGenerator (move-only handle) ──shared_ptr──▶ detail::Engine (heap, never moves)
   │ imageAt(Time) ─── lock decoderMutex ─────────────────▶ ├─ detail::FramePipeline (owns the six below)
   │ generateImages(times, handler) ── enqueue ────────────▶ ├─ deque<shared_ptr<Batch>> + cv
   │ cancelAll()                                            ├─ std::thread worker
   └─ AsyncRequest (copyable) ──shared_ptr──▶ detail::Batch   └─ per item: lock; imageAt(t, token);
                                                                 unlock; handler(Completion)
```

### Structure

The decode machinery is six types plus four value types, all in `stills::detail`, all header-only,
one principal type per header:

| Header | Owns |
|---|---|
| `detail/stills_MediaSource.h` | the `AVFormatContext`, the chosen `AVStream`, `StreamInfo`, the interrupt callback, I/O recovery, the seek primitives, packet reads, re-open |
| `detail/stills_PacketReader.h` | the live `AVPacket`, the keyframe parking slot, the GOP replay buffer, the landing scan, whether the demuxer's keyframe flags can be trusted |
| `detail/stills_KeyframeIndex.h` | recorded keyframes and GOP extents, container-index queries, the B-frame reorder delay |
| `detail/stills_VideoDecoder.h` | the `AVCodecContext`, the hardware device and session, send/receive/flush/drain, the skip policy, the `ActiveDecoder` snapshot |
| `detail/stills_Positioner.h` | every decision about where to send the demuxer — and none of the sending |
| `detail/stills_FramePipeline.h` | orchestration: request validation, the selection loop, end-of-stream policy, the hardware-fault ladder, `AssetInfo`, conversion |

The values they pass around are `FrameSlot` (one owned frame together with `valid` and `concealed`,
as one thing), `DecodeFrontier` (how far the decoder has got and what it never produced, including
`SkippedFrames`), `Position` (where the demuxer was put and what that landing is known to be) and
`SeekCostModel` (what a seek costs and what a frame costs, measured).

**Why the cut is there.** The obvious split — input and decoder setup, positioning and the keyframe
index, decoding and frame selection — does not survive contact with the state. Those three jobs
share a *frontier*: what has been read, what has been fed to the decoder, what has come back out,
and which frames were skipped on purpose. Positioning reads it to choose seek-versus-decode-forward,
selection writes it, recovery invalidates it. Split on responsibility alone and the result is three
classes holding back-pointers to each other — the same coupling, now with somewhere to hide. So the
frontier is named first, owned by the decode loop, and handed to the positioner as a named `const&`:
"reads the frontier" and "writes the frontier" live in the signatures instead of in a comment.

Two of the six then fall out of that argument rather than out of a list of responsibilities.
`PacketReader` exists because the landing scan is 130 lines of packet reading over a byte-level
replay buffer — bookkeeping about packets, not a seek strategy. `KeyframeIndex` exists because it is
the one piece that is a pure data structure, which makes it the one piece with direct unit tests and
no container, no decoder and no file behind them. And input and decoder are two types rather than one
because their lifetimes differ: a single container outlives two or three decoder rebuilds on the
hardware fallback path, and it was `reopen()` rebuilding the codec inline that tied them together.

**Two boundaries worth knowing, because they are not the obvious ones.** `Positioner` decides and
never acts: `canDecodeForwardTo`, `isForwardCheaperThanSeek`, `getIndexedAim`, `getScanAim`,
`nextBackoff` and `retryAfterOvershoot` return aims as data and `FramePipeline` performs them. A seek
here is not a call, it is the invalidation of every other type's state — flush the decoder, empty
three frame slots, reset the frontier, reset the reader's position and the index's contiguity, and
only then record where you aimed — so a type that performed seeks would reach into five others,
which is the orchestrator's job by definition. `PacketReader::readLanding()` reaches the same answer
one size down: it chooses a keyframe and returns "go back to this one" as data. The dependency
therefore runs one way throughout — `FramePipeline` drives the other five — and none of them knows
about its owner or about another's owner.

`Positioner` also holds no references at all; every decision takes a `PositioningView` built at the
call and never stored, for the same reason `KeyframeIndex` takes a `ContainerIndex` by value: a
re-open replaces the `AVFormatContext` and the `AVStream` underneath, and three of the callers sit on
paths that can re-open. A reference member would be a stale-reference hazard waiting for someone to
put a re-open inside a decision.

The test this is held to is *"if I change X, what can break?"*, answerable from the header list
alone. Seek strategy → `Positioner` and `KeyframeIndex`, which hold no frame and no decoder.
Hardware handling → `VideoDecoder` and the ladder in `FramePipeline`, which touch no positioning
state. Frame choice → `FramePipeline` and `FrameSlot`, which read the frontier and cannot write it
behind the positioner's back.

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
`imageAt(1500ms)` and `imageAt(2min)` read naturally with no floating point on the path;
`Time::seconds(double)` is the explicit lossy route. Scaling by a `double` is deliberately **not** an
operator — `t * 1.5` would have truncated to `t * 1` in silence — so that overload is deleted and
the caller writes the rounding. `__int128` is why MSVC is out.

### Image ownership

An `Image` owns the converted `AVFrame` and nothing copies it on the way out. Pixels are exposed as
`std::span` plus `getRowStride()` per plane, because decoder and scaler output is padded and
pretending otherwise is how a consumer gets a sheared picture; `copyPackedTo()` and
`toPackedBytes()` are there for a caller that wants a packed buffer and knows it is paying for
one. Output buffers come from per-geometry `AVBufferPool`s and return to the pool when the `Image`
dies, so an image may safely outlive its generator. Provenance travels with the pixels:
`getActualTime()`, `getDuration()`, `isKeyframe()`, `getAdjustment()`, `isCorrupt()`.

### Resource wrapping

One deleter template, `AvDeleter<Fn>`, dispatches on libav's two free-function shapes — `void(T**)`
and `void(T*)` — and yields `FormatCtxPtr`, `CodecCtxPtr`, `FramePtr`, `PacketPtr`, `BufferRefPtr`,
`SwsCtxPtr` and `DictPtr`, so there is one place in the library that knows how libav frees things.
Creation functions with a `T**` out-parameter are wrapped only *after* success, which closes by
construction the classic libav leak where a half-initialised context is dropped on an error path.
Frames are `av_frame_unref`'d each iteration and moved with `av_frame_move_ref`, never copied; the
codec context owns its `hw_device_ctx` reference and the pipeline owns the hardware device buffer
and the `get_format` state. `detail/stills_FFmpeg.h` is the single libav include point, so the value
headers stay FFmpeg-free and can appear in a consumer's own public headers.

### Concurrency

The engine is heap-allocated and never moves — libav callbacks and the worker hold pointers into it
— and the generator is a move-only handle to a `shared_ptr` of it. One decoder and one worker thread
per generator; options are fixed at `open()` so the worker can never read a field the caller is
mutating, and the two that vary per call travel with the request in `RequestOptions`.

| Operation | Any thread | From inside a completion handler |
|---|---|---|
| `imageAt` | yes — serialised on the decoder | yes |
| `generateImages`, `cancelAll`, `close`, `cancel`, `cancelBatch` | yes | yes |
| `wait` / `waitFor` | yes | returns `refusedOnWorkerThread` at once — waiting there would deadlock |
| `getInfo()`, `getOptions()`, `getActiveDecoder()` | yes | yes |
| destroy the generator | yes, not concurrently with other calls on the same object | yes — teardown completes on the worker |

The worker takes one *item* at a time from the front batch of a single FIFO queue, and a batch stays
at the front until its last item has been dispatched — so items of one batch are delivered in
request order while the queue stays FIFO by batch. Before each item it waits until no synchronous
caller is queued for the decoder, because `std::mutex` is unfair and an `imageAt()` behind a long
batch would otherwise have no bound on how long it waits. That deference is capped at 20 ms: an
*unbounded* priority is the same trap pointing the other way, since threads calling `imageAt()` in
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
| `actualTime` | out-parameter | `Image::getActualTime()` | there is no actual time to read when there is no image |
| `copyCGImage` | throws `NSError` | `std::expected<Image, Error>` | one error channel for the synchronous and asynchronous paths |
| past the end | clamps | `timeOutOfRange`, clamping opt-in | surfaces caller bugs; `OutOfRangePolicy::clampToLastFrame` restores Apple's behaviour |
| `apertureMode`, `videoComposition` | `cleanAperture`; a composition may be applied | neither implemented | clean-aperture cropping is a QuickTime convention and compositing is a different library |

## Known limitations

- One decoder per generator: parallel extraction across far-apart times needs several generators; a
  decoder pool would slot in behind the same API. Options other than the tolerance and the output
  box cannot change after `open()` — the pixel format determines the converter.
- `HardwarePolicy::automatic` is a static table, not a measurement: it cannot know that this
  machine's GPU beats its CPU, or the reverse, and on three device families it now has three
  different right answers for H.264. The intended 1.0 answer is measurement — the codec table as an
  initial guess, then one A/B on real content per *(codec, resolution class, device type)* per
  process, cached and reported in `fallbackReason` — spelled out under
  ["What `automatic` should be"](#what-automatic-should-be). Not counting `get_format`
  renegotiations, which an earlier draft of this README proposed and which is the wrong primary
  signal for the reasons given there. Until it lands, `preferHardware` plus `getActiveDecoder()` is
  the honest override.
- `open()` blocks on all the I/O the demuxer does and cannot be cancelled; bound it with
  `demuxerOptions` such as `rw_timeout`.
- Only right-angle display matrices are honoured (0/90/180/270 plus a horizontal mirror); other
  angles snap to the nearest right angle. 10-bit sources keep their depth only through `p010` and
  `rgba64`. No HDR tone mapping, no deinterlacing, no clean-aperture cropping.
- `Image::getDuration()` is the container's per-frame duration or the average interval; on VFR content
  only `getActualTime()` is authoritative.
- Non-rewindable inputs (`pipe:`) support forward requests only; an earlier time fails with
  `notSeekable` and the generator stays usable. Hardware decode is never attempted on them, because
  validating a candidate decodes the first frame and a rejected candidate needs a rewind.
- Timestamp-less containers (raw H.264/HEVC elementary streams) are decoded forward and re-opened
  for backward requests; frames are stamped in display order from the codec frame rate — libav's 25
  fps default when the stream carries none, flagged by `AssetInfo::timestampsSynthesized`.
- Header-only costs: any TU including `stills_Image.h`, `stills_AssetImageGenerator.h` or the umbrella header also sees
  the libav headers and ~1,000 of their macros, 52 unprefixed (`MKTAG`, `M_PI`, `NAN`, …) — the
  value headers are FFmpeg-free so they can appear in yours instead. These headers are exported as
  `SYSTEM INTERFACE`, so your build sees them through `-isystem` and your warnings do not apply to
  them. If you put them on a plain `-I` path and enable `-Wshadow`, expect ~47 reports from 17
  sites: constructor parameters that share a name with the member they initialise
  (`Time (std::int64_t value, ...) : value (value)`), which is what dropping trailing member
  underscores costs. All are legal and correct: sixteen are constructors whose body is empty, so the
  parameter is the only thing in scope to name, and the seventeenth, `Converter::pooledFrame`,
  shadows a member of a *different* type (`AVPixelFormat` parameter over a `PixelFormat` member), so
  confusing the two would not compile. They are noise in your log, not defects. A plugin embedding stills also
  cannot be unloaded: GCC gives its function-local statics `STB_GNU_UNIQUE` binding and glibc marks
  such objects `NODELETE`, so build one with `-fno-gnu-unique` if it must `dlclose()`.
- Exceptions are used internally though none crosses the API, so `-fno-exceptions` is unsupported
  (`-fno-rtti` is fine). AV1, VVC and DNxHR are expected to work but are untested. ProRes, 10-bit
  HEVC and Matroska were run on real media in the review of this code, with the sampled timestamps
  confirmed against a sequential decode — so those three are no longer on the untested list.
  VideoToolbox has been timed on H.264 in review but not otherwise exercised; NVDEC and VAAPI are
  exercised here; every other hardware family is untested. There is no CI, and the documented build
  has not been run on an Apple toolchain by its author.

## License

MIT — see [LICENSE](LICENSE).

FFmpeg is a separate dependency under its own terms, and those terms follow how *your* FFmpeg was
configured rather than anything here: libav* is LGPL-2.1-or-later as normally built, but a build
configured `--enable-gpl` (x264, x265) is GPL, and `--enable-nonfree` is redistributable not at all.
stills links whatever `pkg-config` finds, so check that build before you ship one.

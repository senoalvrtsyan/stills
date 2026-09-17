# Work Trial — a C++ `AVAssetImageGenerator`

A self-contained library task based on the actual media pipeline work we do.

## The Task

Write a header-only C++23 library wrapped around FFmpeg/libav* that provides an `AVAssetImageGenerator`. This component should extract still frames from a video asset at requested times. You can use Apple's `AVAssetImageGenerator` public documentation as a reference for the intended API and semantics.

You need to support two modes:
- **Synchronous**: Request a time, block until the decoded image is returned.
- **Asynchronous**: Request one or more times, deliver each image as it becomes available, with support for cancellation. Concurrency design is up to you.

## What we're evaluating

Since this is foundational library code, we will read it as your downstream consumers. We are evaluating the public API surface (types, naming, misuse resistance) as much as the internal libav* mechanics. We are looking for:

- Correctness in the FFmpeg decoding loop.
- Resource and lifetime management of C API types.
- Clean error handling.
- Developer UX and API design.

## Design Decisions

We are not defining the internal architecture, edge case handling, or the exact FFmpeg mechanics. You'll need to make decisions on:

- **Video mechanics**: How you implement Apple's concept of accurate seeking using raw `libav*`? Out-of-bounds request handling, corrupted streams, missing tracks etc.
- **Time representation**: Rational, raw timestamps in a timebase, or a rich time type?
- **Image ownership**: How you model output frames and hand pixel ownership back to the caller.
- **Resource wrapping**: How you manage libav* allocations, lifetimes and hardware contexts.
- **Concurrency**: How you safely manage libav* state across concurrent async requests and cancellations.

## Scope & Edge Cases

Core behavior to get right:

- Frame-accurate extraction for any requested time. Offering an alternative "nearest keyframe" mode is a nice-to-have.
- Output configuration: aspect-preserving max size, and caller-chosen pixel format (RGBA minimum).
- Error handling: Design a clean mechanism for handling unsupported inputs/formats, missing streams, edge-case timestamps, etc.
- Clean teardown of in-flight async work upon cancellation.
- Hardware-accelerated decode, with a fallback to software decode.

## Deliverables

1. The header(s) implementing the library.
2. A minimal CMake build for your tests/examples.
3. Tests. This is deterministic and headless, so write failing tests first if you like. You can use a script or FFmpeg command to generate a sample clip (just note it so we can reproduce).

## Constraints

- C++23, header-only.
- Warnings-as-errors.

Submit when you feel the code represents your standard for production library code.

#pragma once
// stills/detail/hooks.hpp — the one libav call the library lets a test replace.
//
// A mid-stream hardware fault rebuilds the decoder once before falling back to software
// (pipeline.hpp, image_at_impl). No driver here fails on demand, so that path is only reachable by
// making one libav call fail: av_hwframe_transfer_data is the one that reports the fault, and it is
// called once per returned hardware frame rather than once per packet. Tests only — the hook is a
// plain global read on the worker thread, so install it before the generator exists (ScopedHook).

#include "stills/detail/config.hpp"
#include "stills/detail/ffmpeg.hpp"

namespace stills::detail {

/// The libav entry points the decode path calls through. Defaults are the libav functions
/// themselves, so a shipped build behaves exactly as if it called them directly.
struct DecoderHooks {
  int (*hwframe_transfer_data)(AVFrame* dst, const AVFrame* src,
                               int flags) = &av_hwframe_transfer_data;
};

/// The process's hooks. One object; see the contract above.
[[nodiscard]] inline DecoderHooks& decoder_hooks() noexcept {
  static DecoderHooks hooks;
  return hooks;
}

/// Installs a hook for a scope and restores the previous one. Tests only.
class ScopedHook {
 public:
  explicit ScopedHook(DecoderHooks replacement) noexcept : saved_(decoder_hooks()) {
    decoder_hooks() = replacement;
  }
  ScopedHook(const ScopedHook&) = delete;
  ScopedHook& operator=(const ScopedHook&) = delete;
  ScopedHook(ScopedHook&&) = delete;
  ScopedHook& operator=(ScopedHook&&) = delete;
  ~ScopedHook() { decoder_hooks() = saved_; }

 private:
  DecoderHooks saved_;
};

}  // namespace stills::detail

#pragma once
// stills/detail/seek_cost_model.hpp — what a seek costs and what a frame costs, measured.
//
// Two exponential averages over the requests this pipeline has served. `seekCostMs` is positioning
// plus the first frame out of a flushed decoder; `frameCostMs` is each further frame. The split
// between them is the moment the first frame came out (noteFirstFrame): everything before it was
// the seek, everything after it was decoding.
//
// The pipeline trades one against the other to decide seek-versus-decode-forward — decoding forward
// wins while the frames up to the covering keyframe cost less than a seek. That trade is real on a
// hardware decoder, where a flush re-initialises the session, and absent on a fast software one,
// which is why it is measured on the caller's machine and the caller's content rather than
// predicted from a table. The same numbers are the right input for deciding whether a hardware
// decoder is worth using at all, in place of the static codec/resolution rule the pipeline uses
// today; that is a change for later, not this pass.
//
// Units are milliseconds throughout, and the clock is steady_clock. A zero cost means "not learned
// yet" and callers check for it, which is why neither average is ever seeded with a guess.
//
// Not thread-safe: a Pipeline is single-threaded by contract (pipeline.hpp).

#include <algorithm>
#include <chrono>
#include <optional>

namespace stills::detail {

class SeekCostModel {
 public:
  // Starts a request: the clock its samples are measured against, and no first frame yet.
  void beginRequest() noexcept {
    requestStartedAt = std::chrono::steady_clock::now();
    firstFrameAt.reset();
  }

  // The first frame of the current request came out of the decoder. Later frames do not move it.
  void noteFirstFrame() noexcept {
    if (!firstFrameAt) firstFrameAt = std::chrono::steady_clock::now();
  }

  // Folds the request that just finished into the averages. `frames` is how many frames the decoder
  // produced for it (look-ahead and skipped frames included), `seeked` whether it positioned by
  // seeking. A request that decoded nothing teaches nothing.
  void learn(int frames, bool seeked) noexcept {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - requestStartedAt).count();
    if (frames <= 0) return;
    if (!seeked) {
      ema(frameCostMs, ms / frames);
      return;
    }
    if (firstFrameAt && *firstFrameAt >= requestStartedAt) {
      ema(seekCostMs,
          std::chrono::duration<double, std::milli>(*firstFrameAt - requestStartedAt).count());
      if (frames > 1) {
        ema(frameCostMs, std::chrono::duration<double, std::milli>(now - *firstFrameAt).count() /
                             (frames - 1));
      }
    } else if (frameCostMs > 0) {
      // No first-frame mark (the frames came from the decoder's own buffer): subtract what the
      // extra frames are known to cost and attribute the rest to the seek.
      ema(seekCostMs, std::max(0.0, ms - (frames - 1) * frameCostMs));
    } else {
      ema(seekCostMs, ms);
    }
  }

  // Zero until the first seeking request has been measured.
  [[nodiscard]] double getSeekCostMs() const noexcept { return seekCostMs; }
  // Zero until the first request that decoded more than the frame it was seeking for.
  [[nodiscard]] double getFrameCostMs() const noexcept { return frameCostMs; }

 private:
  // Weighted towards history (0.7) so one descheduled request does not move the decision.
  static void ema(double& acc, double sample) noexcept {
    acc = acc > 0 ? acc * 0.7 + sample * 0.3 : sample;
  }

  double seekCostMs = 0;
  double frameCostMs = 0;
  std::chrono::steady_clock::time_point requestStartedAt;
  std::optional<std::chrono::steady_clock::time_point> firstFrameAt;
};

}  // namespace stills::detail

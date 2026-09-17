#pragma once
// stills/detail/stills_FrameSlot.h — one decoded frame together with the two flags that describe it.
//
// The pipeline keeps three: the frame it has chosen (held), the one frame of look-ahead that
// proves the held frame covers the request (pending), and the last frame the decoder flagged
// corrupt (a fallback for a stream that produces nothing better). Each owns an AVFrame that stays
// allocated for the life of the pipeline and is unref'd rather than freed, so a slot can be empty
// without giving its buffer back.
//
// The invariant the type exists for: `valid` cannot be written on its own. A slot becomes valid
// only by adopting a frame and invalid only by clear(), so the flag and the frame cannot disagree.
// Before this, the flags were set by hand in five places and agreed only by inspection.
//
// Precondition on every member but install(): the slot has been given its AVFrame.
// FramePipeline::attachToSource() gives all three theirs before anything else can reach them and
// nothing takes one away, so this is a precondition the asserts name, not a case the members
// branch on.
//
// Not thread-safe, and not meant to be: a FramePipeline is single-threaded by contract
// (stills_FramePipeline.h), and a slot is only ever touched by the thread running that
// pipeline's decode loop.

#include <cassert>
#include <utility>

#include "stills/detail/stills_FFmpeg.h"

namespace stills::detail
{

class FrameSlot
{
public:
    FrameSlot() = default;
    // Not movable, on purpose. A defaulted move would leave the source `valid` over a null frame —
    // the same two values disagreeing, in the one direction the accessors cannot report, since
    // isValid() would say yes and getFrame() would hand back nullptr. Nothing moves a slot
    // (FramePipeline owns all three and is itself non-movable), so deleting it costs nothing and
    // closes the hole by construction, which is the whole argument for this type.
    FrameSlot (FrameSlot&&) = delete;
    FrameSlot& operator= (FrameSlot&&) = delete;

    // Installs the AVFrame this slot reuses for its lifetime (allocated by the caller, so allocation
    // failure is reported where the rest of the open path reports it). The slot starts empty, which
    // also means a re-open cannot leave a stale `valid` pointing at a fresh frame. Not called
    // reset(): that is DecodeFrontier's name for throwing state away, which is nearly the opposite.
    void install (FramePtr f) noexcept
    {
        frame = std::move (f);
        valid = false;
        concealed = false;
    }

    [[nodiscard]] bool isValid() const noexcept { return valid; }
    // Whether the decoder concealed errors in this frame, or it was decoded from a suspect reference
    // chain. False while the slot is empty.
    [[nodiscard]] bool isConcealed() const noexcept { return concealed; }
    // nullptr unless valid: an empty slot has no frame to look at.
    [[nodiscard]] AVFrame* getFrame() const noexcept { return valid ? frame.get() : nullptr; }

    // Moves `incoming`'s buffers in, leaving it unref'd and ready to receive again.
    void adopt (AVFrame& incoming, bool wasConcealed) noexcept
    {
        assert (frame && "FrameSlot used before install()");
        av_frame_unref (frame.get());
        av_frame_move_ref (frame.get(), &incoming);
        valid = true;
        concealed = wasConcealed;
    }

    // Promotion between slots: takes `other`'s frame and its concealed flag, and empties it.
    // Precondition: `other` is valid. Promoting an empty slot would mark this one valid over a frame
    // with no buffers — the state this type exists to make unreachable — so it is a precondition
    // rather than a case, and the assert names it in debug builds. Both callers test isValid()
    // first. Not a silent no-op either: a branch here would leave the caller unable to tell whether
    // it holds a frame, and would cost the optimiser the proof that getFrame() is non-null after a
    // promotion.
    void adopt (FrameSlot& other) noexcept
    {
        assert (other.valid && "promoted from an empty FrameSlot");
        adopt (*other.frame, other.concealed);
        other.valid = false;
        other.concealed = false;
    }

    // Drops the frame's buffers; the AVFrame itself stays allocated for the next adopt().
    void clear() noexcept
    {
        assert (frame && "FrameSlot used before install()");
        av_frame_unref (frame.get());
        valid = false;
        concealed = false;
    }

private:
    FramePtr frame;
    bool valid = false;
    bool concealed = false;
};

} // namespace stills::detail

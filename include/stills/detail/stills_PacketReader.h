#pragma once
// stills/detail/stills_PacketReader.h — the packet pump over a MediaSource, and the packets it owns.
//
// PacketReader owns the one AVPacket every read lands in, the parking slot the keyframe scan uses,
// and the GOP replay buffer. It filters the chosen stream out of the demuxer's output, absorbs
// transient demux errors, learns whether the demuxer's keyframe flags can be trusted at all, and
// remembers how far into the stream packets have actually been read. It does not decode, does not
// seek, and does not decide where to position: it is asked for the next packet of the stream and
// produces one.
//
// Why it is its own type. The replay buffer is byte-level bookkeeping — a bounded vector of
// AVPackets scanned past a keyframe so the decoder can be fed them again without re-reading the
// container. That belongs next to the packet it replays into, not inside a seek strategy and not
// inside the decode loop. Packet *ownership* is the same argument from the other side: exactly one
// object may av_packet_move_ref into the live packet and exactly one may unref it.
//
// The packet's lifecycle. getPacket() hands out the live packet by reference; whoever holds it may
// read it and must then either holdPacketForRetry() (the decoder refused it with EAGAIN and it
// must be sent again unchanged) or releasePacket() (it has been consumed). A caller that only
// looked at the packet drops it with unrefPacket(), which leaves the flag alone. The "pending"
// flag and the packet are one thing: a packet is pending exactly when the reader is holding it
// for a retry, and resetPosition() is the only other thing that may drop it.
//
// Not thread-safe: a FramePipeline is single-threaded by contract (stills_FramePipeline.h), and its
// PacketReader is only ever touched by the thread running that pipeline.

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>
#include <vector>

#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_KeyframeIndex.h"
#include "stills/detail/stills_MediaSource.h"
#include "stills/stills_Error.h"

namespace stills::detail
{

class PacketReader
{
public:
    // Allocates the live packet and the parking slot. Runs after every MediaSource open and
    // re-open. The replay buffer is left alone: every path that re-opens reaches readLanding()
    // next, whose first act is clearGopBuffer(), so nothing replays packets read from the container
    // that was closed.
    [[nodiscard]] std::expected<void, Error> attach()
    {
        auto copy = makePacket();

        if (! copy) return std::unexpected (copy.error());
        packet = std::move (*copy);
        packetPending = false;
        auto parkedAllocation = makePacket();

        if (! parkedAllocation) return std::unexpected (parkedAllocation.error());
        parkedPacket = std::move (*parkedAllocation);
        return {};
    }

    // Drops everything tied to where the demuxer was: the replay buffer, the packet held for a
    // retry, the parked keyframe and the run of absorbed demux errors. What the reader has learned
    // about the *stream* (whether keyframe flags are reliable, how far packets have been read)
    // deliberately survives: a reposition does not un-read what was read.
    void resetPosition() noexcept
    {
        clearGopBuffer();
        demuxErrors = 0;

        if (packetPending && packet) av_packet_unref (packet.get());
        packetPending = false;

        if (parkedPacket) av_packet_unref (parkedPacket.get());
    }

    // A re-opened (or grown) source may reach further than the old one did, so what was read from
    // the old one proves nothing about the new one.
    void resetVerifiedTo() noexcept { verifiedTo = libav::noPts; }

    // The live packet. Valid from attach() onwards; the pipeline has none before it opens.
    [[nodiscard]] AVPacket& getPacket() noexcept { return *packet; }
    [[nodiscard]] const AVPacket& getPacket() const noexcept { return *packet; }

    // True when the live packet is being held for a retry: it has not been consumed and must be
    // sent again unchanged.
    [[nodiscard]] bool isPacketPending() const noexcept { return packetPending; }

    // The decoder refused the packet (EAGAIN): keep it, unchanged, for the next send.
    void holdPacketForRetry() noexcept { packetPending = true; }

    // The packet has been consumed, or the retry is being abandoned: unref it and stop holding it.
    void releasePacket() noexcept
    {
        av_packet_unref (packet.get());
        packetPending = false;
    }

    // Drops the live packet's contents and deliberately leaves the pending flag as it was: the
    // caller read this packet only to look at it, and whether a *retry* is owed for some earlier
    // packet is a separate question it has no business answering.
    //
    // The distinction from releasePacket() is not cosmetic. At the keyframe-tail check
    // (FrameSelector::verifyKeyframeTail) the flag can still be set from a send that returned
    // EAGAIN, over a packet this very loop has already overwritten. Clearing it there would be a
    // behaviour change, not a tidy-up: what makes that state safe is that the loop ends with
    // `position.markInvalid()`, so the next request repositions and resetPosition() clears the
    // flag before anything could send the packet. Do not collapse the two without moving that
    // guarantee.
    void unrefPacket() noexcept { av_packet_unref (packet.get()); }

    // Whether the demuxer flags keyframe packets at all. Learned from the very first packet of the
    // stream: a demuxer that never sets the flag must not make the pipeline skip.
    [[nodiscard]] bool areKeyFlagsReliable() const noexcept { return keyFlagsReliable; }

    // The largest packet presentation time actually read from the source, libav::noPts if none. This
    // is evidence that the stream reaches that far, which a decoded frame alone is not.
    [[nodiscard]] std::int64_t getVerifiedTo() const noexcept { return verifiedTo; }

    // Reads the next packet of our stream into the live packet (other streams are dropped), and
    // tells the keyframe index about it. Returns 0, libav::eof (after a live-source growth check),
    // libav::exitRequested or a demux error (transient ones are skipped up to a limit).
    //
    // `tainted` is set — never cleared — when a demux error forced packets to be skipped: there is
    // a hole in what the decoder will be fed, and the caller owns the state that records it.
    //
    // Not noexcept: the keyframe index this records into allocates. A std::bad_alloc here would be
    // std::terminate rather than the ErrorCode::outOfMemory the API can report.
    [[nodiscard]] int readVideoPacket (MediaSource& source, KeyframeIndex& keyframes, bool& tainted,
                                       const CancelToken& token)
    {
        if (isServingReplay())
        {
            // Replayed packets were recorded when they were first read; recording one twice would drag
            // the index's contiguity cursor backwards.
            av_packet_unref (packet.get());
            av_packet_move_ref (packet.get(), gopBuffer[replayPos].get());

            if (++replayPos == gopBuffer.size()) clearGopBuffer();
            return 0;
        }

        const int index = source.getStreamInfo().index;

        for (;;)
        {
            if (token.isRequested()) return libav::exitRequested;
            int result = source.readPacket (*packet);

            if (result == libav::exitRequested) return result;
            if (result == libav::eof || (result < 0 && ++demuxErrors > maxConsecutiveDemuxErrors)) return libav::eof;
            if (result < 0)
            {
                tainted = true;
                continue; // transient demux error: skip and keep reading
            }

            demuxErrors = 0;

            if (packet->stream_index == index && packet->pts != libav::noPts)
            {
                verifiedTo = verifiedTo == libav::noPts ? packet->pts : std::max (verifiedTo, packet->pts);
            }

            if (packet->stream_index != index)
            {
                av_packet_unref (packet.get());
                continue;
            }

            if (! firstPacketSeen)
            {
                // Trust the demuxer's keyframe flags only if it flags the very first packet; a demuxer
                // that never sets the flag must not make us skip.
                firstPacketSeen = true;
                keyFlagsReliable = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            }

            keyframes.notePacket (*packet, containerIndexOf (source));
            return 0;
        }
    }

    // What readLanding() established, plus the two things only the caller may act on.
    struct Landing
    {
        // How the caller must get back to the chosen keyframe before it can be decoded. The scan
        // necessarily reads past that keyframe, so when its GOP was too large to keep in memory
        // someone has to go back to it — and seeking belongs to positioning, not to the reader.
        enum class Rewind
        {
            none,
            byteSeek,
            timestampSeek
        };

        bool found{ false }; // a keyframe packet at or before target is ready (pending or positioned)
        std::int64_t firstKeyPts{ libav::noPts }; // the first keyframe packet seen (> target when !found)
        std::int64_t firstKeyDts{ libav::noPts };
        bool eof{ false };
        // The chosen keyframe's GOP is buffered for replay, so the next packet the decoder sees is
        // that keyframe: the caller's positioning state must say a keyframe is still awaited.
        bool awaitKey{ false };
        Rewind rewind{ Rewind::none };
        KeyEntry rewindEntry{};                // Rewind::byteSeek: the keyframe to seek back to
        std::int64_t rewindTs{ libav::noPts }; // Rewind::timestampSeek: the timestamp to aim at
    };

    // Establishes where a seek landed from the packets, without decoding.
    //
    // Two of its outcomes are the caller's to carry out and are returned rather than performed:
    // `rewind` (a seek, which belongs to positioning) and `awaitKey` (the buffered GOP starts at
    // the chosen keyframe, so the caller's positioning state must still say one is awaited).
    // Ignoring either would feed the decoder a mid-GOP packet stream.
    // FramePipeline::readLanding() is the one caller and does both; anything else that calls this
    // owes them too.
    //
    // `scan == false` (trusted index): the first keyframe packet is the landing, held for the
    // decoder when it is at or before target. `scan == true` (no index): keeps reading through the GOPs
    // up to target, records every keyframe, and settles on the last keyframe at or before target — held for a
    // keyframe-only decode, or reached again with a byte seek for an exact decode.
    [[nodiscard]] std::expected<Landing, Error> readLanding (MediaSource& source, KeyframeIndex& keyframes,
                                                             std::int64_t target, bool scan, bool keyframeMode,
                                                             bool& tainted, const CancelToken& token)
    {
        const ContainerIndex container = containerIndexOf (source);
        const std::int64_t frameDuration = source.getStreamInfo().frameDurationHint;
        const std::int64_t oneFrame = frameDuration > 0 ? frameDuration : std::int64_t{ 0 };
        Landing landing;
        std::int64_t bestPts = libav::noPts, bestDts = libav::noPts, bestPos = -1;
        bool isBestParked = false;              // keyframe mode: the best packet is parked in the reader
        bool buffering = false;                 // exact mode: packets after the best keyframe are kept for replay
        bool scanning = scan;                   // becomes true on a trusted-index container whose seek undershot
        std::int64_t landingPts = libav::noPts; // first packet read after the seek
        // The keyframe-only arming path passes target = INT64_MAX ("the next keyframe, wherever
        // it is"): saturate.
        std::int64_t horizon = 0;

        if (__builtin_add_overflow (target, keyframes.getReorderTicks() + oneFrame, &horizon))
        {
            horizon = std::numeric_limits<std::int64_t>::max();
        }

        clearGopBuffer();

        for (;;)
        {
            const int result = readVideoPacket (source, keyframes, tainted, token);

            if (result == libav::exitRequested)
            {
                if (token.isRequested()) return fail (ErrorCode::cancelled, "cancelled");
                return fail (ErrorCode::decodeFailed, result, "av_read_frame: interrupted without a cancellation");
            }

            if (result == libav::eof)
            {
                landing.eof = true;
                break;
            }

            if (! areKeyFlagsReliable())
            {
                // Cannot tell keyframes apart at the packet level: feed everything, the decoder sorts it
                // out.
                holdPacketForRetry();
                landing.found = true;
                return landing;
            }

            const AVPacket& current = *packet;
            const bool isKey = (current.flags & AV_PKT_FLAG_KEY) != 0;
            const std::int64_t pts =
                current.pts != libav::noPts
                    ? current.pts
                    // Read fresh: notePacket() may have just lowered the delay from this very current.
                    : (current.dts != libav::noPts ? current.dts + keyframes.getReorderTicks() : libav::noPts);

            if (landingPts == libav::noPts && pts != libav::noPts) landingPts = pts;
            if (! isKey)
            {
                if (buffering)
                {
                    // Part of the chosen keyframe's GOP: keep it instead of reading it twice.
                    const bool isPastHorizon = pts != libav::noPts && pts > horizon;

                    if (! bufferPacket()) buffering = false; // over the cap: fall back to a byte seek
                    if (scanning && bestPts != libav::noPts && isPastHorizon)
                        break; // every later packet is past target in decode order too
                    continue;
                }

                // An exact-mode scan is over once a packet past the horizon follows a chosen keyframe. In
                // keyframe mode the scan runs on to the next keyframe so the chosen one's GOP extent is
                // recorded and later requests inside it are answered from the held frame.
                const bool scanIsComplete =
                    scanning && ! keyframeMode && bestPts != libav::noPts && pts != libav::noPts && pts > horizon;

                if (scanIsComplete)
                {
                    unrefPacket();
                    break;
                }

                unrefPacket();
                continue;
            }

            if (pts == libav::noPts)
            {
                holdPacketForRetry(); // a keyframe without a timestamp: nothing to verify against
                landing.found = true;
                return landing;
            }

            if (landing.firstKeyPts == libav::noPts)
            {
                landing.firstKeyPts = pts;
                landing.firstKeyDts = current.dts;
                // No keyframe between the landing and this one: the GOP is at least that long.
                keyframes.noteKeyframeSpan (landingPts, pts);
            }

            if (pts > target)
            {
                if (bestPts != libav::noPts)
                {
                    // The keyframe before this one is the answer; this packet is the next in decode order.
                    if (buffering && ! bufferPacket())
                        buffering = false;
                    else if (! buffering)
                        unrefPacket();
                    break;
                }

                unrefPacket();
                return landing; // overshoot: nothing at or before target was seen
            }

            if (! scanning)
            {
                if (! keyframes.hasKeyBetween (pts, target, container))
                {
                    holdPacketForRetry(); // the landing keyframe covers target
                    landing.found = true;
                    return landing;
                }

                // The index records a later keyframe at or before target (mov lands a GOP early on edit-list
                // files). Read on like a scan: a keyframe past target ends it with the last one at or before target,
                // so a wrong index entry cannot make us overshoot.
                scanning = true;
            }

            bestPts = pts;
            bestDts = current.dts;
            bestPos = current.pos;

            if (keyframeMode)
            {
                parkPacket();
                isBestParked = true;
            }
            else
            {
                clearGopBuffer();
                buffering = bufferPacket(); // the keyframe itself is the first packet to feed
            }
        }

        if (bestPts == libav::noPts)
        {
            clearGopBuffer();
            return landing; // tail of the stream without a keyframe, or nothing at all
        }

        if (landing.eof)
        {
            // The stream ended inside this GOP: its extent is known.
            keyframes.noteStreamEndedInGop (bestPts);
        }

        landing.found = true;

        if (keyframeMode && isBestParked)
        {
            takeParkedPacket();
            return landing;
        }

        if (buffering && hasBufferedGop())
        {
            // The chosen keyframe and its GOP are buffered: decoding replays them.
            landing.awaitKey = true;
            beginReplay();
            return landing;
        }

        clearGopBuffer();

        // GOP too large to buffer: go back to the keyframe by byte position (trusted-index containers
        // refuse byte seeks and re-seek by timestamp below).
        if (bestPos >= 0 && ! container.trusted)
        {
            const KeyEntry* recorded = keyframes.findEntry (bestPts);
            landing.rewind = Landing::Rewind::byteSeek;
            landing.rewindEntry = recorded != nullptr ? *recorded : KeyEntry{ bestPts, bestDts, bestPos, libav::noPts };
            landing.found = true;
            return landing;
        }

        // No byte position (unusual): re-seek by timestamp.
        landing.rewind = Landing::Rewind::timestampSeek;
        landing.rewindTs = bestPts - keyframes.getReorderTicks();
        return landing;
    }

private:
    // True when the next read will be served from the replay buffer rather than the demuxer.
    [[nodiscard]] bool isServingReplay() const noexcept { return replaying && replayPos < gopBuffer.size(); }

    // Keeps the live packet for replay (the landing scan). False when the cap is exceeded, which
    // also empties the buffer: a partial GOP is worse than none, because replaying it would feed
    // the decoder frames whose references were never sent.
    [[nodiscard]] bool bufferPacket()
    {
        const std::size_t bytes = static_cast<std::size_t> (std::max (packet->size, 0));

        if (gopBufferBytes + bytes > maxGopBufferBytes)
        {
            av_packet_unref (packet.get());
            clearGopBuffer();
            return false;
        }

        PacketPtr copy{ av_packet_alloc() };

        if (! copy)
        {
            av_packet_unref (packet.get());
            clearGopBuffer();
            return false;
        }

        av_packet_move_ref (copy.get(), packet.get());
        gopBuffer.push_back (std::move (copy));
        gopBufferBytes += bytes;
        return true;
    }

    void clearGopBuffer() noexcept
    {
        gopBuffer.clear();
        gopBufferBytes = 0;
        replayPos = 0;
        replaying = false;
    }

    // Whether the scan buffered anything to replay.
    [[nodiscard]] bool hasBufferedGop() const noexcept { return ! gopBuffer.empty(); }

    // The scan is over and the buffered GOP is the answer: reads are served from it until it runs
    // out, and the demuxer picks up exactly where the scan left it.
    void beginReplay() noexcept { replaying = true; }

    // Parks the live packet (keyframe mode: the chosen keyframe, while the scan reads on past it).
    void parkPacket() noexcept
    {
        av_packet_unref (parkedPacket.get());
        av_packet_move_ref (parkedPacket.get(), packet.get());
    }

    // Makes the parked keyframe the live packet again, held for the decoder.
    void takeParkedPacket() noexcept
    {
        av_packet_unref (packet.get());
        av_packet_move_ref (packet.get(), parkedPacket.get());
        packetPending = true;
    }

    // Scanned packets of the chosen GOP are kept for replay up to this much; beyond it (4K at high
    // bit rates) the pipeline goes back to the keyframe with a byte seek instead.
    static constexpr std::size_t maxGopBufferBytes = 64u << 20;
    // Matches FrameSelector's decoder error limit; the two count unrelated things.
    static constexpr int maxConsecutiveDemuxErrors = 32;

    PacketPtr packet;
    bool packetPending{ false };      // pkt holds a packet the decoder refused with EAGAIN
    PacketPtr parkedPacket;           // keyframe mode: the chosen keyframe packet while scanning past it
    std::vector<PacketPtr> gopBuffer; // exact mode: the chosen keyframe's packets scanned past target,
                                      // replayed to the decoder
    std::size_t gopBufferBytes{ 0 };
    std::size_t replayPos{ 0 };
    bool replaying{ false }; // the scan is over; readVideoPacket serves gopBuffer first
    int demuxErrors{ 0 };
    bool firstPacketSeen{ false };
    bool keyFlagsReliable{ false };          // the demuxer flags keyframe packets (first packet was flagged)
    std::int64_t verifiedTo{ libav::noPts }; // largest packet presentation time actually read
};

} // namespace stills::detail

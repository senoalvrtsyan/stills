#pragma once
// stills/detail/stills_KeyframeIndex.h — where the keyframes are, and how far a GOP reaches.
//
// KeyframeIndex answers one question in two ways: "which keyframe covers this presentation time?"
// On a container libavformat indexed at open (MP4, Matroska) the answer comes from that index; on
// one it did not (MPEG-TS) the answer comes from the keyframe packets the pipeline has read, which
// this records as it goes, with each GOP's extent filled in once the packets up to the next
// keyframe have been read contiguously. It also owns the two facts that make the two domains
// commensurable: the B-frame reorder delay, and whether keyframe packets carry a DTS at all.
//
// It does not read packets, does not seek and does not decide anything: positioning asks it where
// keyframes are and makes its own choices. The container's own index is reached through a
// ContainerIndex passed per call rather than a reference held as a member, for the reason the
// frontier is passed per call to the positioner — a re-open replaces the AVFormatContext and the
// AVStream underneath, and nothing here may outlive them. That is also what makes this the one
// type in the split with direct unit tests: the recorded-index half needs no container at all.
//
// Timestamp domains, the thing to get right here. Recorded entries are keyed by *presentation*
// time. libavformat's mov index is in the *decode* domain, so a query against it is shifted by
// getIndexShift() (the reorder delay) and its answers are shifted back. Matroska cues are already
// presentation times and its packets carry no DTS, so the shift is zero. Getting this backwards
// places every keyframe one reorder delay out, which looks like a working seek that lands a frame
// early on half the files in the world.
//
// Not thread-safe: a FramePipeline is single-threaded by contract (stills_FramePipeline.h), and its
// KeyframeIndex is only ever touched by the thread running that pipeline.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "stills/detail/stills_FFmpeg.h"
#include "stills/detail/stills_MediaSource.h"

namespace stills::detail
{

// A keyframe packet seen on a container without a trusted index: where it is (byte position for
// AVSEEK_FLAG_BYTE) and, once the packets after it were read contiguously up to the next
// keyframe, where its GOP ends. Sorted by pts.
struct KeyEntry
{
    std::int64_t pts;
    std::int64_t dts;
    std::int64_t pos;
    std::int64_t nextPts; // k::noPts = unknown; INT64_MAX = the stream ended inside this GOP
};

// What a KeyframeIndex query needs from the container it is asked about: libavformat's own index
// for the stream, whether that index can be trusted for this container, and the nominal frame
// duration (the tolerance applied to the index's last entry). Passed per call.
//
// `trusted` and `stream` go together: a trusted index is only ever reported for a real AVStream.
// With `trusted == false` nothing here touches `stream`, which is what lets the recorded-index
// half be exercised with no container at all.
struct ContainerIndex
{
    AVStream* stream{ nullptr };         // non-const: libavformat's index accessors take it that way
    bool trusted{ false };               // StreamInfo::indexTrusted
    std::int64_t frameDurationHint{ 0 }; // StreamInfo::frameDurationHint; 0 = unknown
};

class KeyframeIndex
{
public:
    // Learns the stream's reorder delay (a keyframe's pts - dts; the smallest seen, so open-GOP CRAs
    // with leading pictures do not inflate it) and remembers the stream position of every keyframe
    // packet on containers without a trusted index.
    void notePacket (const AVPacket& p, const ContainerIndex& container)
    {
        const bool key = (p.flags & AV_PKT_FLAG_KEY) != 0;

        if (! key) return;
        if (p.pts != k::noPts && p.dts != k::noPts && p.pts >= p.dts)
        {
            const std::int64_t d = p.pts - p.dts;
            reorderTicks = reorderKnown ? std::min (reorderTicks, d) : d;
            reorderKnown = true;
            keysHaveDts = true;
        }

        recordKey (p.pts, p.dts, p.pos, container);
    }

    // Records a keyframe packet; links it to the previous keyframe when the packets in between were
    // read contiguously (so that keyframe's GOP extent becomes known).
    void recordKey (std::int64_t pts, std::int64_t dts, std::int64_t pos, const ContainerIndex& container)
    {
        if (pts == k::noPts || pos < 0 || container.trusted) return;
        std::size_t i = lowerBound (pts);

        if (i < entries.size() && entries[i].pts == pts)
        {
            entries[i].dts = dts;
            entries[i].pos = pos;
        }
        else
        {
            if (entries.size() >= maxKeyEntries) return;
            entries.insert (entries.begin() + static_cast<std::ptrdiff_t> (i), KeyEntry{ pts, dts, pos, k::noPts });

            if (contigKey != noEntry && contigKey >= i) ++contigKey;
        }

        if (contigKey != noEntry && contigKey < entries.size() && entries[contigKey].pts < pts)
        {
            entries[contigKey].nextPts = pts;
            noteKeyframeSpan (entries[contigKey].pts, pts);
        }

        contigKey = i;
    }

    // The packets after the last recorded keyframe are no longer being read contiguously (the
    // demuxer moved), so the next keyframe recorded says nothing about that one's extent.
    void resetContiguity() noexcept { contigKey = noEntry; }

    // The recorded keyframe whose GOP is known to contain P, or nullptr.
    [[nodiscard]] const KeyEntry* findCoveringKey (std::int64_t P) const noexcept
    {
        if (entries.empty()) return nullptr;
        std::size_t i = lowerBound (P);

        if (i == entries.size() || entries[i].pts != P)
        {
            if (i == 0) return nullptr;
            --i;
        }

        const KeyEntry& e = entries[i];

        if (e.pts > P || e.nextPts == k::noPts || P >= e.nextPts) return nullptr;
        return &e;
    }

    // The recorded entry for exactly this keyframe, or nullptr. Unlike findCoveringKey() this asks
    // nothing about the GOP's extent: the caller has the keyframe and wants its byte position.
    [[nodiscard]] const KeyEntry* findEntry (std::int64_t keyPts) const noexcept
    {
        const std::size_t i = lowerBound (keyPts);

        if (i < entries.size() && entries[i].pts == keyPts) return &entries[i];
        return nullptr;
    }

    // The stream ended inside the GOP that starts at `keyPts`: its extent is known, and reaches
    // past anything that can be asked for. A no-op when that keyframe was never recorded.
    void noteStreamEndedInGop (std::int64_t keyPts) noexcept
    {
        const std::size_t i = lowerBound (keyPts);

        if (i < entries.size() && entries[i].pts == keyPts)
            entries[i].nextPts = std::numeric_limits<std::int64_t>::max();
    }

    // The index entry of the keyframe at or before P on a container with a trusted index, or
    // nullptr when the index does not cover P (a fragmented MP4 whose later fragments have not been
    // read yet). The mov index is in the DTS domain; `reorderTicks` (a keyframe's pts - dts,
    // learned from the first keyframe packet) moves P there. Matroska cues are in the PTS domain and
    // its packets carry no DTS, so the shift is zero.
    [[nodiscard]] const AVIndexEntry* getIndexKeyBefore (std::int64_t P, const ContainerIndex& container) const noexcept
    {
        const int n = avformat_index_get_entries_count (container.stream);

        if (n <= 0) return nullptr;
        const std::int64_t shift = getIndexShift();
        const std::int64_t ts = P > std::numeric_limits<std::int64_t>::min() + shift ? P - shift : P;
        int e = av_index_search_timestamp (container.stream, ts, AVSEEK_FLAG_BACKWARD);

        if (e < 0) return nullptr;
        if (e == n - 1)
        {
            const AVIndexEntry* last = avformat_index_get_entry (container.stream, e);

            if (last == nullptr || ts > last->timestamp + std::max<std::int64_t> (2 * container.frameDurationHint, 1))
                return nullptr;
        }

        for (; e >= 0; --e)
        {
            const AVIndexEntry* entry = avformat_index_get_entry (container.stream, e);

            if (entry == nullptr) return nullptr;
            if ((entry->flags & AVINDEX_KEYFRAME) != 0) return entry;
        }

        return nullptr;
    }

    // True when the container index records a keyframe in (keyPts, P], i.e. `keyPts` is probably
    // not the keyframe covering P. The DTS index is shifted by the *smallest* reorder delay, so an
    // open-GOP I-frame may be placed a frame early: a true result only makes the landing scan on.
    [[nodiscard]] bool hasKeyBetween (std::int64_t keyPts, std::int64_t P,
                                      const ContainerIndex& container) const noexcept
    {
        if (! container.trusted) return false;
        const AVIndexEntry* kf = getIndexKeyBefore (P, container);

        if (kf == nullptr) return false;
        const std::int64_t kfPts = kf->timestamp + getIndexShift();
        return kfPts > keyPts && kfPts <= P;
    }

    // Whether `keyPts` is the keyframe at or before P with no other keyframe in between, known
    // from the container index (MP4/Matroska) or the recorded keyframe index (MPEG-TS).
    [[nodiscard]] bool doesKeyCover (std::int64_t keyPts, std::int64_t P,
                                     const ContainerIndex& container) const noexcept
    {
        if (keyPts > P) return false;
        if (container.trusted)
        {
            const AVIndexEntry* kf = getIndexKeyBefore (P, container);

            if (kf == nullptr) return false;
            // mov indexes DTS (a keyframe's pts is its dts plus the reorder delay); Matroska cues are
            // PTS.
            return kf->timestamp + getIndexShift() == keyPts;
        }

        const KeyEntry* e = findCoveringKey (P);
        return e != nullptr && e->pts == keyPts;
    }

    // Offset from the container index's timestamp domain to presentation time: the reorder delay
    // when the index (and the keyframe packets) carry DTS (mov), zero for a PTS index (Matroska).
    [[nodiscard]] std::int64_t getIndexShift() const noexcept { return keysHaveDts ? reorderTicks : 0; }

    // Picks whichever of a packet's two timestamps is in the container index's domain.
    [[nodiscard]] std::int64_t pickIndexTs (std::int64_t dts, std::int64_t pts) const noexcept
    {
        return keysHaveDts ? dts : pts;
    }

    // The B-frame reorder delay: a keyframe's pts - dts, the smallest seen. Zero until a keyframe
    // packet carrying both has been noted.
    [[nodiscard]] std::int64_t getReorderTicks() const noexcept { return reorderTicks; }

    // A lower bound on the longest GOP in the stream, in stream ticks, 0 until something has been
    // observed. Positioning uses it to aim a seek early enough and to bound a forward scan.
    [[nodiscard]] std::int64_t getGopHint() const noexcept { return gopHint; }

    // Widens the GOP estimate from a span that provably contains no keyframe boundary: `from` and
    // `to` are presentation times with nothing keyframe-flagged strictly between them. Callers
    // measure such a span from keyframe packets (the landing scan, the recorded index) or from
    // decoded frames (the selection loop); the estimate does not distinguish them, because both are
    // the same lower bound on how far apart keyframes are and a lower bound only ever grows. A
    // span that is not longer than what is already known leaves the estimate alone.
    void noteKeyframeSpan (std::int64_t from, std::int64_t to) noexcept
    {
        if (from == k::noPts || to == k::noPts || to <= from) return;
        gopHint = std::max (gopHint, to - from);
    }

private:
    static constexpr std::size_t noEntry = static_cast<std::size_t> (-1);
    static constexpr std::size_t maxKeyEntries = 1u << 20;

    [[nodiscard]] std::size_t lowerBound (std::int64_t pts) const noexcept
    {
        std::size_t lo = 0, hi = entries.size();

        while (lo < hi)
        {
            const std::size_t mid = lo + (hi - lo) / 2;

            if (entries[mid].pts < pts)
                lo = mid + 1;
            else
                hi = mid;
        }

        return lo;
    }

    std::vector<KeyEntry> entries;    // keyframe packets seen (containers without a trusted index)
    std::size_t contigKey{ noEntry }; // entry of the last keyframe read without a seek since (its
                                      // GOP extent grows)
    std::int64_t gopHint{ 0 };        // largest keyframe spacing observed, in stream ticks
    std::int64_t reorderTicks{ 0 };   // a keyframe's pts - dts (the B-frame reorder delay),
                                      // smallest seen
    bool reorderKnown{ false };
    bool keysHaveDts{ false }; // keyframe packets carry a dts (mov); Matroska's do not
};

// The container-index view of a source, as KeyframeIndex's queries take it. Built per call and
// never stored: a re-open replaces both the AVFormatContext and the AVStream underneath.
[[nodiscard]] inline ContainerIndex containerIndexOf (const MediaSource& source) noexcept
{
    const StreamInfo& info = source.getStreamInfo();
    return ContainerIndex{ source.getStream(), info.indexTrusted, info.frameDurationHint };
}

} // namespace stills::detail

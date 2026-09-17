#!/usr/bin/env bash
# Generates the deterministic test clips. Usage: generate.sh <ffmpeg-binary> <output-dir>
#
# Frame identity: every frame's luma encodes its 0-based index N so tests can recover it from any
# output format after any decoder/scaler:
#   left half  Y = 22 + 14 * (N % 16)
#   right half Y = 22 + 14 * (N / 16)
# Values 22..232 stay inside limited range, so RGB conversion never clips.
# 64x48 @ 30 fps, 4 s = 120 frames, keyframes forced at exactly 0/30/60/90, B-frames enabled
# (I B B P ... reordering), High profile 4:2:0 -> decodable by every hardware decoder.
#
# Kept bash-3 compatible (macOS /bin/bash): arrays, $(( )), head -c, wc -c | tr, dd conv=notrunc.
set -euo pipefail
FF="${1:-ffmpeg}"
OUT="${2:-fixtures}"
mkdir -p "$OUT"
run() { "$FF" -y -hide_banner -loglevel error "$@"; }

# ffmpeg 6.0 or newer: -display_rotation, -display_hflip/-display_vflip and -fps_mode are used
# below and did not exist before it. Without this check an older binary produces fixtures that are
# quietly wrong (unrotated, constant frame rate) and the failures land in the tests instead.
FF_VERSION=$("$FF" -version 2>/dev/null | head -1 | sed -n 's/^ffmpeg version n\{0,1\}\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\1 \2/p')
if [ -z "$FF_VERSION" ]; then
  echo "warning: cannot read the version of '$FF'; assuming it is 6.0 or newer" >&2
else
  FF_MAJOR=${FF_VERSION% *}
  if [ "$FF_MAJOR" -lt 6 ]; then
    echo "error: $FF is version $FF_MAJOR; the fixtures need ffmpeg 6.0 or newer" >&2
    echo "       (-display_rotation, -display_hflip/-display_vflip and -fps_mode)" >&2
    exit 1
  fi
fi

SRC="color=c=black:size=64x48:rate=30:duration=4,format=yuv420p,geq=lum='if(lt(X\,W/2)\,22+14*mod(N\,16)\,22+14*floor(N/16))':cb=128:cr=128"
X264=(-c:v libx264 -profile:v high -crf 10 -g 30 -force_key_frames "expr:gte(t,n_forced)"
      -x264-params "bframes=2:b-adapt=0:scenecut=0")

# 1. primary clip (MP4, time_base 1/15360 -> frame k has pts k*512)
run -f lavfi -i "$SRC" "${X264[@]}" -movflags +faststart "$OUT/counter.mp4"
# 2. MPEG-TS with start_time = 10 s (time_base 1/90000). Seeks land on arbitrary packets (DTS-based
#    generic seek), so every backward request exercises the landing verification.
run -i "$OUT/counter.mp4" -c copy -muxdelay 0 -output_ts_offset 10 -f mpegts "$OUT/counter_offset.ts"
# 2b. MPEG-TS with a single GOP (one IDR at frame 0): every seek away from the start lands with no
#     keyframe ahead, forcing the back-off to the explicit start seek.
run -f lavfi -i "$SRC" -c:v libx264 -profile:v high -crf 10 -g 120 -x264-params "bframes=2:b-adapt=0:scenecut=0" \
    -f mpegts "$OUT/counter_longgop.ts"
# 2c. Matroska: 1 ms time base, so frame k is stored at round(k * 1000 / 30) ms — the request
#     rounding case.
run -i "$OUT/counter.mp4" -c copy "$OUT/counter.mkv"
# 2d. WebM/VP9 (same 1 ms time base, different codec/decoder). Skipped when libvpx is missing.
ENCODERS=$("$FF" -hide_banner -encoders 2>/dev/null || true)
case "$ENCODERS" in
  *libvpx-vp9*)
    run -f lavfi -i "$SRC" -c:v libvpx-vp9 -crf 10 -b:v 0 -g 30 -auto-alt-ref 1 "$OUT/counter_vp9.webm" ;;
esac
# 3. display-matrix rotation (90 degrees; ffmpeg's autorotate shows it upright)
run -display_rotation 90 -i "$OUT/counter.mp4" -c copy "$OUT/counter_rot90.mp4"
# 4. variable frame rate: every third source frame kept (source index still in the luma)
run -i "$OUT/counter.mp4" -vf "select='not(mod(n\,3))'" -fps_mode vfr -c:v libx264 -crf 10 "$OUT/counter_vfr.mp4"
# 4b. variable frame rate with a long-held NON-REFERENCE frame: 16 of every
#     100 source frames are kept, so frame 15 (a non-reference B under bframes=3, no pyramid) stays on
#     screen from 0.5 s to 3.33 s while the average interval is 1/30 s. The mov muxer stores decode-order
#     deltas as sample durations, so every packet still says "1/30 s": the skip decision must not
#     trust durations or the average frame rate.
run -f lavfi -i "color=c=black:size=64x48:rate=30:duration=20,format=yuv420p,geq=lum='if(lt(X\,W/2)\,22+14*mod(N\,16)\,22+14*floor(N/16))':cb=128:cr=128" \
    -vf "select='lt(mod(n\,100)\,16)'" -fps_mode vfr -c:v libx264 -profile:v high -crf 10 -x264-params "bframes=3:b-adapt=0:b-pyramid=none:scenecut=0" \
    -movflags +faststart "$OUT/counter_vfr_hold.mp4"
# 5. audio only
run -f lavfi -i "sine=frequency=440:duration=1" -c:a aac "$OUT/audio_only.m4a"
# 6. single image (image2/png_pipe demuxer, unknown duration)
run -f lavfi -i "color=c=red:size=32x32:duration=0.04" -frames:v 1 "$OUT/red.png"
# 7. corrupt (512 random bytes written into the media data; moov is at the front thanks to faststart)
SZ=$(wc -c < "$OUT/counter.mp4" | tr -d ' ')
cp "$OUT/counter.mp4" "$OUT/counter_corrupt.mp4"
dd if=/dev/urandom of="$OUT/counter_corrupt.mp4" bs=1 seek=$((SZ * 3 / 4)) count=512 conv=notrunc 2>/dev/null
# 8. truncated (last quarter missing)
head -c $((SZ * 3 / 4)) "$OUT/counter.mp4" > "$OUT/counter_trunc.mp4"
# 9. not a video
printf 'this is not a video\n' > "$OUT/not_a_video.mp4"
# 10. anamorphic: same frames, sample aspect ratio 2:1 (display 128x48)
run -i "$OUT/counter.mp4" -vf setsar=2 "${X264[@]}" -movflags +faststart "$OUT/counter_sar2.mp4"
# 11. the other display-matrix rotations, and the two mirrors
run -display_rotation 180 -i "$OUT/counter.mp4" -c copy "$OUT/counter_rot180.mp4"
run -display_rotation 270 -i "$OUT/counter.mp4" -c copy "$OUT/counter_rot270.mp4"
run -display_hflip -i "$OUT/counter.mp4" -c copy "$OUT/counter_hflip.mp4"
run -display_vflip -i "$OUT/counter.mp4" -c copy "$OUT/counter_vflip.mp4"
# 12. raw Annex-B elementary stream: no container timestamps, no index, unknown duration
run -i "$OUT/counter.mp4" -c copy -bsf:v h264_mp4toannexb -f h264 "$OUT/counter.h264"
# 13. audio with cover art (attached picture) — not a video unless Options::allow_attached_pictures
run -i "$OUT/audio_only.m4a" -i "$OUT/red.png" -map 0:a -map 1:v -c:a copy -c:v png -disposition:v:0 attached_pic "$OUT/cover.m4a"
# 14. video is stream 1 (audio first)
run -i "$OUT/audio_only.m4a" -i "$OUT/counter.mp4" -map 0:a -map 1:v -c copy -shortest "$OUT/av_audio_first.mp4"
# 15. edit list: copy from 1.1 s; the first *presented* frame (index 33) is not a keyframe
run -ss 1.1 -i "$OUT/counter.mp4" -c copy "$OUT/counter_editlist.mp4"
# 16. RGBA source (PNG in MOV): half-transparent blue, full-range RGB in, colour-range labelling out
run -f lavfi -i "color=c=blue@0.5:size=64x48:rate=30:duration=1,format=rgba" -c:v png "$OUT/alpha.mov"
# 17. odd dimensions (33x17 RGB): sizing rules per output format
run -f lavfi -i "color=c=red:size=33x17:rate=30:duration=1,format=rgb24" -c:v png "$OUT/odd.mov"
# 18. fragmented MP4 (every fragment starts at a keyframe). The mov demuxer does not apply its
#     PTS->DTS reorder shift to fragmented files, so a plain seek to the last frames of a GOP lands
#     on the *next* fragment.
run -i "$OUT/counter.mp4" -c copy -movflags frag_keyframe+empty_moov+default_base_moof "$OUT/counter_frag.mp4"
# 18b. mid-stream profile switch: 4 s of High 4:2:0 followed by 4 s of lossless
#      High 4:4:4 (frames 120..239 in the luma index) in one MPEG-TS. Hardware decoders accept the
#      first segment and fail on the second: exercises the rebuild-once-then-software fallback.
#      The MP4 -> TS remux shifts the first segment by its 2-frame reorder delay, so the second
#      segment starts at 4 s + 2/30 s to keep the timeline gapless.
run -f lavfi -i "color=c=black:size=64x48:rate=30:duration=4,format=yuv444p,geq=lum='if(lt(X\,W/2)\,22+14*mod(N+120\,16)\,22+14*floor((N+120)/16))':cb=128:cr=128" \
    -c:v libx264 -qp 0 -g 30 -x264-params "bframes=2:b-adapt=0:scenecut=0" -muxdelay 0 -output_ts_offset 4.0666667 -f mpegts "$OUT/counter_444.ts"
run -i "$OUT/counter.mp4" -c copy -muxdelay 0 -f mpegts "$OUT/counter_seg0.ts"
run -i "concat:$OUT/counter_seg0.ts|$OUT/counter_444.ts" -c copy -f mpegts "$OUT/counter_profile_switch.ts"
# 20. 10-bit source (High 10 profile): every 8-bit output is dithered from it, p010/rgba64 keep the depth.
run -f lavfi -i "$SRC" -c:v libx264 -profile:v high10 -pix_fmt yuv420p10le -crf 10 -g 30 -x264-params "bframes=2:b-adapt=0:scenecut=0" \
    -movflags +faststart "$OUT/counter_p10.mp4"
# 21. open-GOP H.264 (x264 open-gop: B-frames before a non-IDR I-frame reference across it) and
#     interlaced H.264 (field-coded MBAFF): keyframe flags and the decoded index must stay exact.
run -f lavfi -i "$SRC" -c:v libx264 -profile:v high -crf 10 -g 30 -x264-params "open-gop=1:bframes=2:b-adapt=0:scenecut=0" \
    -movflags +faststart "$OUT/counter_opengop.mp4"
run -f lavfi -i "$SRC" -c:v libx264 -profile:v high -crf 10 -g 30 -flags +ilme+ildct -x264-params "interlaced=1:bframes=2:b-adapt=0:scenecut=0" \
    -movflags +faststart "$OUT/counter_ilace.mp4"
# 22. raw MJPEG (NOTIMESTAMPS, intra-only, full-range yuvj420p): a 25 fps timeline is synthesised.
run -f lavfi -i "$SRC" -c:v mjpeg -q:v 2 -f mjpeg "$OUT/counter.mjpeg"
# 23. rotated odd-sized RGB source: the rotation kernel on 33x17 RGB and 32x16 4:2:0 outputs.
run -display_rotation 90 -i "$OUT/odd.mov" -c copy "$OUT/odd_rot90.mov"
# 24. deterministic corruption: a fixed 512-byte pattern written over the media data
#     at a fixed offset (the last quarter of the file: frames from about index 90 on).
cp "$OUT/counter.mp4" "$OUT/counter_corrupt_fixed.mp4"
python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256)) * 2)" 2>/dev/null | dd of="$OUT/counter_corrupt_fixed.mp4" bs=1 seek=$((SZ * 3 / 4)) count=512 conv=notrunc 2>/dev/null || \
  head -c 512 /dev/zero | tr '\0' '\252' | dd of="$OUT/counter_corrupt_fixed.mp4" bs=1 seek=$((SZ * 3 / 4)) count=512 conv=notrunc 2>/dev/null
# 25. two recordings concatenated into one MPEG-TS, the second stamped 40 s *earlier*: the
#     timestamps jump backwards mid-file. Decoding forward across the jump ends on a frame from the
#     earlier segment, so the last frame decoded is not the last frame in presentation order.
run -i "$OUT/counter.mp4" -c copy -muxdelay 0 -output_ts_offset 50 -f mpegts "$OUT/disc_a.ts"
run -i "$OUT/counter.mp4" -c copy -muxdelay 0 -output_ts_offset 10 -f mpegts "$OUT/disc_b.ts"
cat "$OUT/disc_a.ts" "$OUT/disc_b.ts" > "$OUT/disc.ts"
rm -f "$OUT/disc_a.ts" "$OUT/disc_b.ts"
# 26. a small file declaring an enormous frame: 8192x8192 (67.1 megapixels) in a couple of hundred
#     kilobytes. Setting up a decoder for it and probing its first frame costs about a gigabyte of
#     resident memory, which is what Options::max_input_pixels exists to refuse.
run -f lavfi -i "color=c=black:size=8192x8192:rate=30:duration=0.2" \
    -c:v libx264 -preset ultrafast -crf 30 -pix_fmt yuv420p "$OUT/huge8k.mp4"
# 19. open-GOP HEVC (x265 default): the two frames before each CRA are leading pictures that follow
#     it in decode order and are dropped when decoding starts at the CRA. Skipped without libx265.
case "$ENCODERS" in
  *libx265*)
    run -f lavfi -i "$SRC" -c:v libx265 -crf 10 -x265-params "open-gop=1:keyint=30:min-keyint=30:bframes=2:b-adapt=0:scenecut=0:log-level=error" \
        -movflags +faststart "$OUT/counter_opengop_hevc.mp4" ;;
esac
echo "fixtures written to $OUT"

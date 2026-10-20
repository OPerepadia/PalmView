#!/usr/bin/env bash
# Merges downloaded streams with pview and with ffmpeg, then compares every
# packet (pts, dts, size, flags) and the decoded video of both outputs.
#
#   host/tests/compare-merge.sh PVIEW DIR VIDEO_ID...
#
# DIR must hold VIDEO_ID.video.mp4 and VIDEO_ID.audio.mp4 (pview get).
set -euo pipefail

pview="$1"
dir="$2"
shift 2
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
failed=0

for id in "$@"; do
  video="$dir/$id.video.mp4"
  audio="$dir/$id.audio.mp4"
  "$pview" merge "$video" "$audio" "$work/ours.mp4" >/dev/null 2>&1
  ffmpeg -v error -y -i "$video" -i "$audio" -map 0:v -map 1:a -c copy \
    -movflags +faststart "$work/ref.mp4"
  result="ok"
  for stream in 0 1; do
    for file in ours ref; do
      ffprobe -v error -select_streams "$stream" \
        -show_entries packet=pts,dts,size,flags -of csv=p=0 \
        "$work/$file.mp4" >"$work/$file.$stream.csv"
    done
    cmp -s "$work/ours.$stream.csv" "$work/ref.$stream.csv" ||
      result="packets differ in stream $stream"
  done
  ffmpeg -v error -i "$work/ours.mp4" -f null - 2>"$work/decode.log"
  [[ -s "$work/decode.log" ]] && result="decode errors"
  ours_md5="$(ffmpeg -v error -i "$work/ours.mp4" -map 0 -f md5 -)"
  ref_md5="$(ffmpeg -v error -i "$work/ref.mp4" -map 0 -f md5 -)"
  [[ "$ours_md5" == "$ref_md5" ]] || result="decoded output differs"
  echo "$id: $result"
  [[ "$result" == ok ]] || failed=1
done
exit "$failed"

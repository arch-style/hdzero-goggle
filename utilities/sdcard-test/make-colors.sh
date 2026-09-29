#!/bin/sh
# 色の見え方を比べるための動画を作る。録画と同じ形 (HEVC Main 1280x720 60fps,
# AAC 無音, MPEG-TS, 1 秒 GOP) で 20 秒。HD のカラーバー (白・黄・シアン・緑・
# マゼンタ・赤・青と灰色の階調) の上にコマ番号を出す。
#
#   utilities/sdcard-test/make-colors.sh [出力先]   (既定は ./colors.ts)
#
# カード直下に置き、autotest から /mnt/extsd/colors.ts として使う。
set -e

OUT=${1:-colors.ts}
FONT=/System/Library/Fonts/Supplemental/Arial\ Bold.ttf

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "smptehdbars=s=1280x720:r=60:d=20" \
    -f lavfi -i "anullsrc=r=48000:cl=stereo" \
    -filter_complex "[0:v]drawtext=fontfile=$FONT:fontsize=64:fontcolor=white:box=1:boxcolor=black:x=40:y=40:text='%{eif\:n\:d}'[v]" \
    -map "[v]" -map 1:a -t 20 \
    -c:v libx265 -preset fast -pix_fmt yuv420p \
    -x265-params "keyint=60:min-keyint=60:bframes=0:log-level=error" -b:v 6M \
    -c:a aac -b:a 64k -ar 48000 -ac 2 \
    -muxdelay 0 -muxpreload 0 -f mpegts "$OUT"

echo "$OUT"

#!/bin/sh
# 60fps の動きが画面で何コマずつ出ているかを目で見るための動画を作る。録画と同じ形
# (HEVC Main 1280x720 60fps, AAC 無音, MPEG-TS, 1 秒 GOP) で 20 秒。
# 白い縦棒が 1 コマに 8px ずつ右へ流れ、画面中央にコマ番号を出す。60Hz で全コマ
# 出ていれば棒は滑らかに流れ、50Hz (6 コマに 1 コマが出ない) だと周期的に跳ねる。
#
#   utilities/sdcard-test/make-motion.sh [出力先] [720|1080]   (既定は ./motion.ts, 720)
#
# カード直下に置き、autotest から /mnt/extsd/motion.ts として使う。1080 を渡すと
# 1920x1080 で作る (プレイヤーは 1080p60 の表示にする。720p60 表示との比較用)。
set -e

OUT=${1:-motion.ts}
if [ "${2:-720}" = 1080 ]; then
    W=1920 H=1080 STEP=12
else
    W=1280 H=720 STEP=8
fi
FONT=/System/Library/Fonts/Supplemental/Arial\ Bold.ttf

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "color=c=0x202020:s=${W}x${H}:r=60:d=20" \
    -f lavfi -i "color=c=white:s=24x${H}:r=60:d=20" \
    -f lavfi -i "anullsrc=r=48000:cl=stereo" \
    -filter_complex "[0:v][1:v]overlay=x='mod(n*${STEP}\,${W})':y=0:eval=frame,drawtext=fontfile=$FONT:fontsize=120:fontcolor=0x00c0ff:x=(w-tw)/2:y=(h-th)/2-200:text='%{eif\:n\:d}'[v]" \
    -map "[v]" -map 2:a -t 20 \
    -c:v libx265 -preset fast -pix_fmt yuv420p \
    -x265-params "keyint=60:min-keyint=60:bframes=0:log-level=error" -b:v 6M \
    -c:a aac -b:a 64k -ar 48000 -ac 2 \
    -muxdelay 0 -muxpreload 0 -f mpegts "$OUT"

echo "$OUT"

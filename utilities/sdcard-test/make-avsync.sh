#!/bin/sh
# 音と映像の同期を目と耳で確かめるための動画を作る。録画と同じ形
# (HEVC Main 1280x720 60fps, AAC-LC 48kHz ステレオ, MPEG-TS, 1 秒 GOP)で 30 秒。
# 毎秒 2 コマの白フラッシュと、同じ pts に 1kHz 40ms のビープ。画面中央に
# 秒.コマ を出す。AAC のプライミングで映像も音も 21.3ms 後ろにずれるが、両方
# 同じだけなので同期は崩れない。
#
#   utilities/sdcard-test/make-avsync.sh [出力先]   (既定は ./avsync.ts)
#
# カード直下に置き、autotest.avsend.txt から /mnt/extsd/avsync.ts として使う。
set -e

OUT=${1:-avsync.ts}
FONT=/System/Library/Fonts/Supplemental/Arial\ Bold.ttf
BEEP="if(lt(mod(t\,1)\,0.04)\,0.2*sin(2*PI*1000*t)\,0)"

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "color=c=0x181818:s=1280x720:r=60:d=30" \
    -f lavfi -i "aevalsrc='$BEEP|$BEEP':s=48000:d=30" \
    -filter_complex "[0:v]drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='lt(mod(n\,60)\,2)',drawtext=fontfile=$FONT:fontsize=160:fontcolor=0x00c0ff:x=(w-tw)/2:y=(h-th)/2:text='%{eif\:floor(n/60)\:d}.%{eif\:mod(n\,60)\:d\:2}'[v]" \
    -map "[v]" -map 1:a \
    -c:v libx265 -preset fast -pix_fmt yuv420p \
    -x265-params "keyint=60:min-keyint=60:bframes=0:log-level=error" -b:v 6M \
    -c:a aac -b:a 128k -ar 48000 -ac 2 \
    -muxdelay 0 -muxpreload 0 -f mpegts "$OUT"

echo "$OUT"

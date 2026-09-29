#!/bin/sh
# 60Hz 再生（FPGA のライブ側）で特定の明るさだけが化ける件を調べる動画。
# 録画と同じ形 (HEVC Main 1280x720 60fps, AAC 無音, MPEG-TS) で 10 秒の静止画。
# 横方向に明るさがゆっくり変わる帯が上から 6 本:
#
#   1  Y 110->140  色なし (U=V=128)       化ける値を 1 段ずつ数えられる幅
#   2  Y 110->140  U=131                   色が少しでもあると化けないか
#   3  Y 110->140  V=131
#   4  Y   0->255  色なし                  化ける値がほかにもあるか
#   5  Y   0->30   色なし                  黒の近く
#   6  Y 225->255  色なし                  白の近く
#
# 各帯の左端に番号。ほぼ劣化なしで圧縮する (qp=1) ので、デコードした値は作った
# 値からずれても 1。
#
#   utilities/sdcard-test/make-keytest.sh [出力先]   (既定は ./keytest.ts)
#
# カード直下に置き、autotest から /mnt/extsd/keytest.ts として使う。
set -e

OUT=${1:-keytest.ts}
FONT=/System/Library/Fonts/Supplemental/Arial\ Bold.ttf

# Luma rows: band = floor(Y/120). Chroma planes are half size: band = floor(Y/60).
LUM="if(lt(Y,360),110+30*X/W,if(lt(Y,480),255*X/W,if(lt(Y,600),30*X/W,225+30*X/W)))"
CB="if(between(Y,60,119),131,128)"
CR="if(between(Y,120,179),131,128)"

LABELS=""
for i in 1 2 3 4 5 6; do
    y=$(( (i - 1) * 120 + 4 ))
    LABELS="$LABELS,drawtext=fontfile=$FONT:fontsize=28:fontcolor=white:box=1:boxcolor=black:boxborderw=4:x=4:y=$y:text=$i"
done

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "color=c=black:s=1280x720:r=60:d=10,format=yuv420p" \
    -f lavfi -i "anullsrc=r=48000:cl=stereo" \
    -filter_complex "[0:v]geq=lum='$LUM':cb='$CB':cr='$CR'$LABELS[v]" \
    -map "[v]" -map 1:a -t 10 \
    -c:v libx265 -preset fast -pix_fmt yuv420p \
    -x265-params "keyint=60:min-keyint=60:bframes=0:qp=1:log-level=error" \
    -c:a aac -b:a 64k -ar 48000 -ac 2 \
    -muxdelay 0 -muxpreload 0 -f mpegts "$OUT"

echo "$OUT"

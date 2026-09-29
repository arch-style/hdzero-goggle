#!/bin/sh
# 自動テストの grab で取り込んだ画像 (カードの grab/*.nv21) を PNG にして開く。
# 大きさはファイルの大きさから決める (1920x1080 か 1280x720 の NV21)。
#
#   utilities/sdcard-test/grab-to-png.sh [取り込みのフォルダ] [出力先]
#
# フォルダの既定はカードの grab。名前だけ (grab.2 など) ならカード直下のそれ。
# 出力先の既定は ~/Downloads/ の下の同じ名前のフォルダ。
set -e

CARD="/Volumes/NO NAME"
DIR=${1:-grab}
case $DIR in
*/*) ;;
*) DIR="$CARD/$DIR" ;;
esac
OUT=${2:-"$HOME/Downloads/$(basename "$DIR")"}

mkdir -p "$OUT"
for f in "$DIR"/*.nv21; do
    [ -e "$f" ] || { echo "no .nv21 in $DIR"; exit 1; }
    case $(stat -f %z "$f") in
    3110400) s=1920x1080 ;;
    1382400) s=1280x720 ;;
    *) echo "skipped $(basename "$f"): not a whole frame"; continue ;;
    esac
    ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt nv21 -s $s -i "$f" \
        "$OUT/$(basename "$f" .nv21).png"
done
echo "$OUT"
open "$OUT"

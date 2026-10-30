#!/bin/sh
# 純正の disp.ko に 4 バイトのパッチを当て、画面切り替えの 1 秒待ちを短くした
# 写しを作る (README.md)。書き換えたバイナリはリポジトリに置かず、手元で作る。
#
#   make-disp-ko.sh [純正の disp.ko] [出力先] [待ち ms (1-255、既定 100)]
#
# 純正の既定は mkapp/app/ko/disp.ko (本家がリポジトリに入れている 9.5.1 の
# disp.ko、md5 01f5f9a0...)。ゴーグルの /mnt/app/ko/disp.ko の写しでもよい。
# 出力の既定はこのフォルダの disp.ko (.gitignore 済み)。
# 書き換えるのは disp_vdpo_disable() の最後の mov.w r0, #1000 (4f f4 7a 70) を
# mov.w r0, #<ms> (4f f0 <ms> 00) にする 4 バイトだけ。

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
IN=${1:-$ROOT/mkapp/app/ko/disp.ko}
OUT=${2:-$HERE/disp.ko}
MS=${3:-100}

STOCK_MD5=01f5f9a0adf42fee6b159f35113cff89
PATCHED_MD5=$(cut -d' ' -f1 < "$HERE/disp.ko.md5")
OFF=$((0x12eac))

md5_of() {
	if command -v md5 > /dev/null 2>&1; then md5 -q "$1"; else md5sum "$1" | cut -d' ' -f1; fi
}
bytes_at() {
	dd if="$1" bs=1 skip="$OFF" count=4 2> /dev/null | od -An -tx1 | tr -d ' \n'
}

case "$MS" in
	*[!0-9]* | '') echo "待ちは 1-255 の数で (mov.w の 8 ビット即値)" >&2; exit 1 ;;
esac
if [ "$MS" -lt 1 ] || [ "$MS" -gt 255 ]; then
	echo "待ちは 1-255 の数で (mov.w の 8 ビット即値)" >&2
	exit 1
fi

if [ "$(md5_of "$IN")" != "$STOCK_MD5" ]; then
	echo "$IN は純正 9.5.1 の disp.ko ではない (md5 $(md5_of "$IN"))" >&2
	exit 1
fi
if [ "$(bytes_at "$IN")" != 4ff47a70 ]; then
	echo "$IN の 0x12eac が mov.w r0, #1000 ではない ($(bytes_at "$IN"))" >&2
	exit 1
fi

TMP="$OUT.tmp.$$"
cp "$IN" "$TMP"
printf "\\117\\360\\$(printf '%03o' "$MS")\\000" |
	dd of="$TMP" bs=1 seek="$OFF" count=4 conv=notrunc 2> /dev/null

if [ "$(bytes_at "$TMP")" != "$(printf '4ff0%02x00' "$MS")" ]; then
	echo "書き込んだ 4 バイトが違う ($(bytes_at "$TMP"))" >&2
	rm -f "$TMP"
	exit 1
fi
if [ "$MS" = 100 ] && [ "$(md5_of "$TMP")" != "$PATCHED_MD5" ]; then
	echo "できた disp.ko の md5 が disp.ko.md5 と違う ($(md5_of "$TMP"))" >&2
	rm -f "$TMP"
	exit 1
fi
mv "$TMP" "$OUT"
echo "$OUT: 待ち $MS ms (md5 $(md5_of "$OUT"))"

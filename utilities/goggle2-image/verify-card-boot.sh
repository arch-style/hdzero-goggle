#!/bin/sh
# カードに書いた起動領域を読み戻して、layout.txt の md5 と比べる (読むだけ)。
# 使い方: sudo sh verify-card-boot.sh /dev/rdiskN <出力ディレクトリ>/layout.txt
dev=$1; layout=$2
[ -r "$layout" ] || { echo "usage: $0 /dev/rdiskN layout.txt"; exit 2; }
rc=0
grep -v '^#' "$layout" | while read -r name start count want; do
	got=$(dd if="$dev" bs=512 skip="$start" count="$count" 2>/dev/null | md5 -q)
	if [ "$got" = "$want" ]; then echo "ok   $name (sectors $start +$count)"; else echo "DIFF $name (sectors $start +$count): card $got expected $want"; rc=1; fi
done
exit $rc

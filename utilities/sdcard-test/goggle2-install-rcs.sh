#!/bin/sh
# Goggle 2 の上書き領域に、起動時間の実験用の rcS の写し (v3) を入れ直す (一度だけ)。
# PhoenixCard のあと、SSH が使えない場所で使う。rc-exp.sh が記録の終わりに g2 を付けて呼ぶ。
# 効くのは次の起動から。
#
# 書くもの: /etc/init.d/rcS。読み出し専用の純正 rcS の上に、上書き領域 (/overlay/upperdir) の
# ファイルとしてできる。中身は goggle2-flash/rcS.v3 (md5 fc9058fe)。2026-10-17 と 2026-10-20 に
# SSH で書いたのと同じ手順 (隣に .new を書き、md5 と sh -n を確かめてから mv)。
# いまの rcS が純正 (a98ac80e)、v1 (ca520542)、v2 (258268277f) のときだけ書く。すでに v3 なら
# 何もしない。それ以外なら触らない。
# 元に戻すのは /overlay/upperdir/etc/init.d/rcS を消す。/etc/init.d/rcS を消してはいけない
# (純正まで隠れて起動しなくなる)。
# 終わると goggle2-install-rcs/ に result.txt と DONE ができる。DONE があれば次の起動からは何もしない。
STOCK=a98ac80ea33aebcc473b651e95a21b9c
V1=ca520542a353ed7ea7c72636caf43710
V2=258268277f4ab2de326a0befb8f594ed
V3=fc9058fef07d9c13debf0107734d3ceb
R=/mnt/extsd/goggle2-install-rcs
SRC=/mnt/extsd/goggle2-flash/rcS.v3
DST=/etc/init.d/rcS
[ -f "$SRC" ] || exit 0
[ -e "$R/DONE" ] && exit 0
if [ "$1" != g2 ]; then
	PLATFORM=$(cat /mnt/app/platform 2>/dev/null)
	if [ -n "$PLATFORM" ]; then
		[ "$PLATFORM" = HDZGOGGLE2 ] || exit 0
	else
		grep -qi goggle2 /mnt/app/app/HDZGOGGLE || exit 0
	fi
fi
mkdir -p "$R" 2>/dev/null || exit 0
( : > "$R/.wtest" ) 2>/dev/null || exit 0
rm -f "$R/.wtest"
L="$R/result.txt"
say() {
	echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$L"
	sync
}
m() { md5sum < "$1" 2>/dev/null | cut -d' ' -f1; }

if [ "$(m "$SRC")" != "$V3" ]; then
	say "skip: カードの goggle2-flash/rcS.v3 の md5 が合わない ($(m "$SRC"))"
	date > "$R/DONE"; sync
	exit 0
fi
if ! grep -qs overlay /proc/mounts; then
	say "skip: 上書き領域がマウントされていない"
	exit 0
fi
cur=$(m "$DST")
if [ "$cur" = "$V3" ]; then
	say "rcS: すでに v3 ($cur)"
	date > "$R/DONE"; sync
	exit 0
fi
if [ "$cur" != "$STOCK" ] && [ "$cur" != "$V1" ] && [ "$cur" != "$V2" ]; then
	say "rcS: 純正でも実験の写しでもない ($cur)。触らない"
	date > "$R/DONE"; sync
	exit 0
fi
cp "$DST" "$R/rcS.before"
say "before $cur upperdir: $(ls /overlay/upperdir/etc/init.d/ 2>/dev/null | tr '\n' ' ')"
if ! cp "$SRC" "$DST.new"; then
	rm -f "$DST.new"; sync
	say "fail: $DST.new を書けない。元のまま"
	date > "$R/DONE"; sync
	exit 0
fi
chmod 775 "$DST.new"; sync
if [ "$(m "$DST.new")" = "$V3" ] && sh -n "$DST.new"; then
	mv "$DST.new" "$DST" && sync
	say "rcS: 書いた $(m "$DST") upperdir: $(m /overlay/upperdir/etc/init.d/rcS)"
else
	rm -f "$DST.new"; sync
	say "fail: 書いた rcS.new が合わない。元のまま"
fi
cp "$DST" "$R/rcS.after" 2>/dev/null
date > "$R/DONE"
sync
exit 0

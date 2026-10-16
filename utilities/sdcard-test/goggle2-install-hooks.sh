#!/bin/sh
# Goggle 2 のフラッシュに、起動時間の実験用の入口 2 つを入れ直す (一度だけ)。
# PhoenixCard や SD からの緊急更新のあと、SSH が使えない場所で使う。
# develop.sh (develop.sh.dual) の Goggle 2 側から、アプリを起動する前に呼ぶ。
#
# 書くもの (どちらも中身は goggle2-flash/ の写しで、入口の数行を足しただけ):
#   /mnt/app/script/rc.sh      カードに rc-exp.sh があればそれを実行する入口
#   /etc/init.d/rc.final       (上書き領域に置かれる) カードに init-exp.sh が
#                              あれば S スクリプトごとに呼ぶ入口
# 書く前に、いまの中身が純正 9.5.1 か、すでに入口入りかを md5 で確かめる。
# どちらでもなければ触らない。隣に書いてから名前を変えるので、途中で電源が
# 落ちても古いか新しいかのどちらかになる。
#
# カードに置くもの:
#   goggle2-install-hooks.sh   これ
#   goggle2-flash/rc.sh        入口入りの rc.sh (md5 は下の HOOKED_RC)
#   goggle2-flash/rc.final     入口入りの rc.final (md5 は下の HOOKED_FINAL)
# 終わると goggle2-install-hooks/ に result.txt と DONE ができる。DONE があれば
# 次の起動からは何もしない。元に戻すのは goggle2-flash/*.orig を同じ手順で
# 書き戻すか、SD からの緊急更新。

STOCK_RC=5fe8e7e1775d7a07f29c6c53b171883b
HOOKED_RC=cb87f2580bfce48f3198f94401454d7e
STOCK_FINAL=07c372b5b13ea55dc9b35e69484d1fac
# rc.final: 入口だけの版 (2026-10-16) と、それに起動の段の印を足した版 (2026-10-17)。
# 入口だけの版は、純正と同じく新しい版へ上げてよいものとして扱う。
PREV_FINAL=cbc2056aa493be98a8832c185a2b8b70
HOOKED_FINAL=efe8f15333a125490c5f55834eb6b041

R=/mnt/extsd/goggle2-install-hooks
SRC=/mnt/extsd/goggle2-flash
[ -f "$SRC/rc.sh" ] && [ -f "$SRC/rc.final" ] || exit 0
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

# カードの写しが壊れていないか。
if [ "$(m "$SRC/rc.sh")" != "$HOOKED_RC" ] || [ "$(m "$SRC/rc.final")" != "$HOOKED_FINAL" ]; then
	say "skip: カードの goggle2-flash/rc.sh か rc.final の md5 が合わない"
	date > "$R/DONE"; sync
	exit 0
fi

# 1. /mnt/app/script/rc.sh
DST=/mnt/app/script/rc.sh
cur=$(m "$DST")
if [ "$cur" = "$HOOKED_RC" ]; then
	say "rc.sh: すでに入口入り"
elif [ "$cur" != "$STOCK_RC" ]; then
	say "rc.sh: 純正でも入口入りでもない ($cur)。触らない"
else
	cp "$DST" "$R/rc.sh.before"
	cp "$SRC/rc.sh" "$DST.new" && chmod 775 "$DST.new" && chown 1002 "$DST.new" 2>/dev/null; sync
	if [ "$(m "$DST.new")" = "$HOOKED_RC" ] && sh -n "$DST.new"; then
		mv "$DST.new" "$DST" && sync
		say "rc.sh: 書いた $(m "$DST")"
	else
		rm -f "$DST.new"; sync
		say "rc.sh: 書いた rc.sh.new が合わない。元のまま"
	fi
fi

# 2. /etc/init.d/rc.final (上書き領域)
DST=/etc/init.d/rc.final
cur=$(m "$DST")
if [ "$cur" = "$HOOKED_FINAL" ]; then
	say "rc.final: すでに入口入り"
elif [ "$cur" != "$STOCK_FINAL" ] && [ "$cur" != "$PREV_FINAL" ]; then
	say "rc.final: 純正でも入口入りでもない ($cur)。触らない"
else
	cp "$DST" "$R/rc.final.before"
	cp "$SRC/rc.final" "$DST.new" && chmod 775 "$DST.new"; sync
	if [ "$(m "$DST.new")" = "$HOOKED_FINAL" ] && sh -n "$DST.new"; then
		mv "$DST.new" "$DST" && sync
		say "rc.final: 書いた $(m "$DST") (上書き領域 $(ls /overlay/upperdir/etc/init.d/ 2>/dev/null | tr '\n' ' '))"
	else
		rm -f "$DST.new"; sync
		say "rc.final: 書いた rc.final.new が合わない。元のまま"
	fi
fi
say "done: rc.sh=$(m /mnt/app/script/rc.sh) rc.final=$(m /etc/init.d/rc.final)"
date > "$R/DONE"
sync

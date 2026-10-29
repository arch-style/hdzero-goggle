#!/bin/sh
# Goggle 2 の setting.ini を、カードに置いた控えで一度だけ書き戻す。
# 緊急更新 (HDZG_OS.bin) で設定が初期値に戻ったときに使う。develop.sh
# (develop.sh.dual) の Goggle 2 側から、アプリを起動する前に同期で呼ぶ。
#
# 書くのは /mnt/app/setting.ini だけ。メニューで設定を変えたときにアプリが
# 書くのと同じファイルで、他のフラッシュには触らない。
#
# カードの /mnt/extsd/fork-system/goggle2-restore/ に置くもの (Mac で用意する):
#   setting.ini       書き戻す中身 (goggle2-backup/setting.ini の写し)
#   setting.ini.md5   その md5。カードの中で壊れていたら書かない
# 終わると同じ場所にできるもの:
#   setting.ini.before  書き戻す前の /mnt/app/setting.ini
#   setting.ini.after   書き戻した後に /mnt/app から読み直したもの
#   result.txt          経過と結果
#   DONE                最後に書く。これがあれば次の起動からは何もしない

R=/mnt/extsd/fork-system/goggle2-restore
SRC="$R/setting.ini"
DST=/mnt/app/setting.ini
[ -f "$SRC" ] || exit 0
[ -e "$R/DONE" ] && exit 0

# Goggle 2 のときだけ。見分け方は develop.sh.dual と同じ。develop.sh.dual は
# 見分けた結果を引数 g2 で渡すので、そのときは見分け直さない (純正アプリの
# grep に 1.1 秒かかる)。
if [ "$1" != g2 ]; then
	PLATFORM=$(cat /mnt/app/platform 2>/dev/null)
	if [ -n "$PLATFORM" ]; then
		[ "$PLATFORM" = HDZGOGGLE2 ] || exit 0
	else
		grep -qi goggle2 /mnt/app/app/HDZGOGGLE || exit 0
	fi
fi
( : > "$R/.wtest" ) 2>/dev/null || exit 0
rm -f "$R/.wtest"

L="$R/result.txt"
say() {
	echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$L"
	sync
}

want=$(cut -d' ' -f1 < "$R/setting.ini.md5" 2>/dev/null)
have=$(md5sum < "$SRC" | cut -d' ' -f1)
if [ -z "$want" ] || [ "$want" != "$have" ]; then
	say "skip: カードの setting.ini の md5 が合わない (want=$want have=$have)"
	exit 0
fi
if ! grep -q '^\[settings\]' "$SRC"; then
	say "skip: setting.ini に [settings] が無い"
	exit 0
fi

cp "$DST" "$R/setting.ini.before" 2>/dev/null
say "before $(md5sum < "$DST" | cut -d' ' -f1)"

# 隣に書いてから名前を変える。途中で電源が落ちても、setting.ini は古いか
# 新しいかのどちらかで、半端にはならない。
cp "$SRC" "$DST.new" && sync
if [ "$(md5sum < "$DST.new" | cut -d' ' -f1)" != "$want" ]; then
	rm -f "$DST.new"
	sync
	say "fail: 書いた setting.ini.new が合わない。元のまま"
	exit 0
fi
mv "$DST.new" "$DST" && sync

after=$(md5sum < "$DST" | cut -d' ' -f1)
cp "$DST" "$R/setting.ini.after"
if [ "$after" = "$want" ]; then
	say "ok $after"
else
	say "fail: 書き戻した後の md5 が違う ($after)"
fi
date > "$R/DONE"
sync

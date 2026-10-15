#!/bin/sh
#
# goggle2-backup.sh が取った控えを確かめて Mac に移す (Mac 側で実行)。
#
#   utilities/sdcard-test/goggle2-backup-check.sh /Volumes/<SDカード>
#
# 見るもの:
#   1. 最後まで取れたこと (DONE)
#   2. 各パーティションの写しが /proc/mtd の大きさどおりで、合計が NOR の 32MB
#   3. 写しの md5 が、ゴーグルがデバイスを読み直して取った md5 と同じ
#   4. mnt_app.tar に setting.ini が入っている
# 全部通れば logs/goggle2/<日付>-flash-backup/ に写し、写した先も md5 で確かめる。
# カードの控えは消さない。何か引っかかれば非 0 で終わる。

SD=${1:?usage: goggle2-backup-check.sh /Volumes/<SDカード>}
B="$SD/goggle2-backup"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
fail=0

ng() { printf 'NG  %s\n' "$*"; fail=1; }
ok() { printf 'ok  %s\n' "$*"; }

if [ ! -d "$B" ]; then
	echo "NG  $B が無い (まだ Goggle 2 で起動していない?)"
	exit 1
fi
cat "$B/progress.txt" 2>/dev/null | sed 's/^/  /'
echo

if [ -f "$B/DONE" ]; then
	ok "最後まで取れている ($(cat "$B/DONE"))"
else
	ng "DONE が無い: 途中で止まった。progress.txt の最後の行が止まった場所"
	exit 1
fi

# info.txt の "== mtd" の節から、名前ごとの大きさを取る。
total=0
for f in "$B"/mtd[0-7]-*.bin; do
	[ -e "$f" ] || { ng "mtd の写しが無い"; break; }
	base=$(basename "$f")
	n=${base%%-*}
	hex=$(awk -v n="$n:" '/^== mtd$/ {s=1; next} /^== / {s=0} s && $1 == n {print $2}' "$B/info.txt")
	want=$((0x$hex))
	got=$(stat -f %z "$f")
	total=$((total + got))
	if [ "$got" -eq "$want" ]; then
		ok "$base $got bytes"
	else
		ng "$base $got bytes (パーティションは $want bytes)"
	fi
	dev=$(awk -v f="$base" '$2 == f {print $1}' "$B/md5-device.txt")
	if [ "$(md5 -q "$f")" = "$dev" ]; then
		ok "  md5 がデバイスの読み直しと一致"
	else
		ng "  md5 がデバイスの読み直しと違う (カード $(md5 -q "$f"), デバイス ${dev:-なし})"
	fi
done
if [ "$total" -eq 33554432 ]; then
	ok "合計 32MB (NOR の全体)"
else
	ng "合計 $total bytes (32MB = 33554432 のはず)"
fi

if tar -tf "$B/mnt_app.tar" 2>/dev/null | grep -qx 'app/setting.ini'; then
	ok "mnt_app.tar に setting.ini がある"
else
	ng "mnt_app.tar に setting.ini が無い"
fi
if cmp -s "$B/setting.ini" "$B/setting.ini.last-boot"; then
	ok "setting.ini は最後の起動時と同じ"
else
	printf '    setting.ini は控えた後に変わった。最新は setting.ini.last-boot\n'
fi
echo

if [ $fail -ne 0 ]; then
	echo "NG: Mac には写さない。カードの goggle2-backup/ を消して取り直す"
	exit 1
fi

DEST="$ROOT/logs/goggle2/$(date +%Y-%m-%d)-flash-backup"
if [ -e "$DEST" ]; then
	echo "$DEST がすでにある。上書きしないので、確かめるだけにする"
else
	mkdir -p "$DEST" && cp -p "$B"/* "$DEST"/ || { echo "NG: 写せなかった"; exit 1; }
fi
for f in "$B"/*; do
	if ! cmp -s "$f" "$DEST/$(basename "$f")"; then
		ng "$DEST/$(basename "$f") がカードと違う"
	fi
done
if [ $fail -eq 0 ]; then
	echo "OK: $DEST に控えた。カードと同一"
fi
exit $fail

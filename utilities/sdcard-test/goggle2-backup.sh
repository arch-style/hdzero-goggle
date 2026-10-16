#!/bin/sh
# Goggle 2 の設定と、SoC のフラッシュ (NOR 32MB) 全体をカードに写す。読むだけ。
# 緊急更新 (HDZG_OS.bin) は設定のあるアプリ領域ごと書き直すので、その前に
# 一度だけ取っておく。develop.sh (develop.sh.dual) の Goggle 2 側から、アプリを
# 起動する前に同期で呼ぶ。取っている間は映像が出ない (初回だけ、数十秒)。
#
# 結果は /mnt/extsd/goggle2-backup/。v1 の控え (goggle-backup/) とは分けてある。
#   setting.ini            初回の設定
#   setting.ini.last-boot  毎回の起動で取り直す (DONE があっても)
#   mnt_app.tar            /mnt/app をファイル単位で
#   overlay_upper.tar      純正の OS に上書きされているファイル (/overlay/upperdir)
#   mtdN-<名前>.bin        パーティションの生の写し。uboot から UDISK までの 8 個で
#                          NOR の全体になる
#   md5-device.txt         写し終えた後にデバイスをもう一度読んで取った md5。
#                          Mac で .bin の md5 と比べると、読み取りとカードへの
#                          書き込みの両方を確かめられる (goggle2-backup-check.sh)
#   info.txt               版、カーネル、mtd 構成、マウント、u-boot の環境
#   progress.txt           各段階の経過秒。途中で止まったときの手がかり
#   DONE                   最後に書く。これがあれば次の起動からは取らない
# 取り直すときは Mac に移してから goggle2-backup/ ごと消す。
#
# 外付けの RX / VA のフラッシュ (w25q128.ko で見える mtd8 以降) は読まない。
# この時点ではドライバが入っておらず、入れると動いている FPGA と SPI を
# 取り合う。中身は公式の HDZGOGGLE_RX.bin / HDZGOGGLE_VA.bin で書き直せる。

# Goggle 2 のときだけ。見分け方は develop.sh.dual と同じ。v1 は借り物なので
# 読むだけでも動かさない。develop.sh.dual は見分けた結果を引数 g2 で渡すので、
# そのときは見分け直さない。純正アプリを grep するのに 1.1 秒かかり、起動が
# その分遅れていた (2026-10-16 に測定)。
if [ "$1" != g2 ]; then
	PLATFORM=$(cat /mnt/app/platform 2>/dev/null)
	if [ -n "$PLATFORM" ]; then
		[ "$PLATFORM" = HDZGOGGLE2 ] || exit 0
	else
		grep -qi goggle2 /mnt/app/app/HDZGOGGLE || exit 0
	fi
fi

( : > /mnt/extsd/.wtest ) 2>/dev/null || exit 0
rm -f /mnt/extsd/.wtest

B=/mnt/extsd/goggle2-backup
mkdir -p "$B" || exit 0
cp /mnt/app/setting.ini "$B/setting.ini.last-boot"
# 起動時間の実験 (develop.sh.dual が SYNC_BG=1 を渡す) では、ここの sync を
# 裏に回す。
if [ "$SYNC_BG" = 1 ]; then
	sync &
else
	sync
fi
[ -e "$B/DONE" ] && exit 0

P="$B/progress.txt"
step() {
	echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$P"
	sync
}

# 写しと tar で 50MB 弱。足りなければ何もしない。
free_kb=$(df -k /mnt/extsd | awk 'NR==2 {print $4}')
if [ -z "$free_kb" ] || [ "$free_kb" -lt 102400 ]; then
	step "skip: free ${free_kb}KB"
	exit 0
fi

step "start"
cp /mnt/app/setting.ini "$B/setting.ini"

{
	echo "== version";    cat /mnt/app/version 2>&1
	echo "== platform";   cat /mnt/app/platform 2>&1
	echo "== uname";      uname -a
	echo "== cmdline";    cat /proc/cmdline
	echo "== mtd";        cat /proc/mtd
	echo "== mounts";     cat /proc/mounts
	echo "== df";         df -k
	echo "== fw_printenv"; fw_printenv 2>&1
	echo "== md5";        md5sum /mnt/app/app/HDZGOGGLE /mnt/app/ko/*.ko /mnt/app/app/record/record 2>&1
	echo "== ls /mnt/app"; ls -laR /mnt/app 2>&1
	echo "== ls /overlay/upperdir"; ls -laR /overlay/upperdir 2>&1
} > "$B/info.txt" 2>&1
step "info"

tar -cf "$B/mnt_app.tar" -C /mnt app 2>> "$P"
step "mnt_app.tar rc=$?"
tar -cf "$B/overlay_upper.tar" -C /overlay upperdir 2>> "$P"
step "overlay_upper.tar rc=$?"

# 生の写し。名前は /proc/mtd から取る。
: > "$B/md5-device.txt"
LIST=""
while read -r dev size erase name; do
	case "$dev" in
	mtd[0-9]*:) ;;
	*) continue ;;
	esac
	n=${dev%:}
	[ "${n#mtd}" -le 7 ] || continue
	name=$(echo "$name" | tr -d '"')
	f="$n-$name.bin"
	cat "/dev/$n" > "$B/$f"
	step "$f $(wc -c < "$B/$f") bytes (mtd 0x$size)"
	LIST="$LIST $n:$f"
done < /proc/mtd
sync

# 二度目の読み取り。カードの写しではなくデバイスの md5 を残す。
for e in $LIST; do
	n=${e%%:*}
	f=${e#*:}
	echo "$(md5sum < "/dev/$n" | cut -d' ' -f1)  $f" >> "$B/md5-device.txt"
done
step "md5"

date > "$B/DONE"
step "done"

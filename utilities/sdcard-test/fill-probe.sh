#!/bin/sh
# Goggle 2: SD カードを使うほど重くなる処理を、カードの今の中身で測る (2026-10-18)。
# カードの直下に置くと、rc-exp.sh の記録の終わり (ビープの前) がこれを 1 回呼ぶ。
# 止めるときは fill-probe.sh.off に名前を変える。
#
#   /bin/sh /mnt/extsd/fill-probe.sh <段階> <起動番号>
#
# 測るもの (それぞれ直前に sync + drop_caches で、起動直後と同じく
# ディレクトリがまだ読まれていない状態から):
#   sdstat   純正の sdstat_log_backup.sh が次の番号を探すループ (19-36 行目の写し)。
#            純正の rc.sh ではアプリの起動前に終わるまで待たされる。
#   create   そのフォルダに長い名前のファイルを 1 本作って消す (sdstat の cp の代わり)。
#   ls       readdir + ファイルごとの stat (録画プロセスの disk_countMovies に近い)。
#   fsck     fsck.fat -n (読むだけ、直さない)。純正アプリが起動のたびに行う点検
#            (chkfixsd.sh の fsck.fat -y) の代わり。本物は外してから走るので、
#            1 つ目の FAT もカードから読み直す分 (このカードで約 0.12 秒) と、
#            sleep 3 秒が足される。
# 対象: カードの log/、DCIM/100HDZRO、boot-logs/、直下と、Mac の fill-test.sh が作る
# filltest/ の下のフォルダ (log-<本数>、dcim-<本数>)。
# 書くのは boot-logs/fill-stage<段階>.txt と rc-exp.log の 1 行と、create の一時
# ファイル (すぐ消す) だけ。フラッシュには書かない。drop_caches は動いている間だけの
# もので、電源を切れば関係ない。
# 時刻は /proc/uptime (10 ms 単位) を fork せずに読む。

STAGE=${1:-0}
BOOTN=${2:-0}
SD=/mnt/extsd
DEL=$SD/fork-deletable
OUT=$DEL/boot-logs/fill-stage$STAGE.txt
C=/sys/devices/system/cpu/cpu0/cpufreq

now() {
	read u _ < /proc/uptime
	NOW=$(( ${u%.*} * 100 + 1${u#*.} - 100 ))
}
# 計測ごとに最低クロックを最高に固定し直す (上書き領域の rcS が 15 秒で元に戻すため)。
cold() {
	cat $C/scaling_max_freq 2>/dev/null > $C/scaling_min_freq
	sync
	echo 3 2>/dev/null > /proc/sys/vm/drop_caches
}
put() {
	echo "$*" >> "$OUT"
}
nfiles() {
	n=0
	for f in "$1"/* "$1"/.[!.]*; do [ -e "$f" ] && n=$((n + 1)); done
	echo $n
}

# 純正 sdstat_log_backup.sh の 19-36 行目と同じ (同じ sh の中で回すので fork しない)。
sdstat_loop() {
	index=-1
	for file in $1/*.log.*
	do
		if [ -f "$file" ]
		then
			let index_i=${file##*.}
			if [ "${index_i}" -gt "${index}" ]
			then
				index=${index_i}
			fi
		fi
	done
}

probe_sdstat() {
	cold
	now; t0=$NOW
	sdstat_loop "$1"
	now; t1=$NOW
	# 純正はこの後、同じフォルダに <名前>.log.<番号> を cp する。
	echo x > "$1/zz-fill-probe.log.999999" 2>/dev/null
	now; t2=$NOW
	rm -f "$1/zz-fill-probe.log.999999"
	put "probe sdstat dir=$1 files=$(nfiles "$1") ms=$(( (t1 - t0) * 10 )) create_ms=$(( (t2 - t1) * 10 )) max_index=$index"
}

probe_ls() {
	cold
	now; t0=$NOW
	ls -l "$1" > /dev/null 2>&1
	now; t1=$NOW
	put "probe ls dir=$1 files=$(nfiles "$1") ms=$(( (t1 - t0) * 10 ))"
}

probe_root() {
	cold
	now; t0=$NOW
	# 純正 rc.sh のサービスの確認 (ls -1 /mnt/extsd/hdzgoggle-services-*) と同じ glob
	for f in $SD/hdzgoggle-services-*; do :; done
	now; t1=$NOW
	put "probe root dir=$SD files=$(nfiles "$SD") ms=$(( (t1 - t0) * 10 ))"
}

probe_fsck() {
	FSCK=${FILL_FSCK:-/bin/fsck.fat}
	[ -x "$FSCK" ] || { put "probe fsck skipped (no $FSCK)"; return; }
	BLK=${FILL_BLK:-/dev/mmcblk0p1}
	[ -b "$BLK" ] || [ -n "$FILL_BLK" ] || BLK=/dev/mmcblk0
	cold
	now; t0=$NOW
	"$FSCK" -n "$BLK" > /tmp/fill-fsck.log 2>&1 &
	pid=$!
	# 2 分で打ち切る (読むだけなので打ち切っても何も残らない)。
	( sleep 120; kill "$pid" 2>/dev/null ) &
	wd=$!
	wait "$pid"
	rc=$?
	now; t1=$NOW
	kill "$wd" 2>/dev/null
	put "probe fsck dev=$BLK ms=$(( (t1 - t0) * 10 )) rc=$rc lines=$(wc -l < /tmp/fill-fsck.log)"
	# 最後の行が "<dev>: <n> files, <使用>/<全部> clusters"
	head -20 /tmp/fill-fsck.log | sed 's/^/fsck: /' >> "$OUT"
	tail -2 /tmp/fill-fsck.log | sed 's/^/fsck-tail: /' >> "$OUT"
}

now; T0=$NOW
put "--- boot $BOOTN stage $STAGE fill-probe start $u cur=$(cat $C/scaling_cur_freq 2>/dev/null) min=$(cat $C/scaling_min_freq 2>/dev/null)"
df /mnt/extsd 2>/dev/null | tail -1 | sed 's/^/df: /' >> "$OUT"

[ -d $SD/log ] && probe_sdstat $SD/log
[ -d $SD/DCIM/100HDZRO ] && probe_ls $SD/DCIM/100HDZRO
[ -d $DEL/boot-logs ] && probe_ls $DEL/boot-logs
probe_root

for d in $SD/filltest/log-*; do
	[ -d "$d" ] && probe_sdstat "$d"
done
for d in $SD/filltest/dcim-*; do
	[ -d "$d" ] && probe_ls "$d"
done

probe_fsck

now
put "fill-probe end $u cur=$(cat $C/scaling_cur_freq 2>/dev/null) total_ms=$(( (NOW - T0) * 10 ))"
echo "$u fill probe done ($(( (NOW - T0) * 10 )) ms, boot-logs/fill-stage$STAGE.txt)" >> $DEL/rc-exp.log
exit 0

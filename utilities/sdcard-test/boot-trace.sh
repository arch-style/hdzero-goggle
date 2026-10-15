#!/bin/sh
# 起動の内訳を取る。develop.sh の先頭から、develop.sh に入った時点の
# /proc/uptime を引数にして呼ぶ:
#
#   ENTRY=$(cut -d' ' -f1 /proc/uptime)
#   [ -e /mnt/extsd/boot-trace.sh ] && /bin/sh /mnt/extsd/boot-trace.sh "$ENTRY" &
#
# 何も変えない。読むだけで、結果をカードの boot-trace/NNN/ に置く。
# フラッシュにも設定にも触らない。読む側は boot-trace-report.py。
#
# 取るもの:
#   uptime.txt   develop.sh に入った時点と、この採取を始めた時点の経過秒
#   dmesg.txt    カーネルログ (printk の時刻付き)。カーネルがいつ init に
#                渡したか、どのドライバが遅いかが分かる
#   procs.txt    生きているプロセス全部の開始時刻 (起動からの秒)。init、
#                rc.sh、record、system_daemon がいつ始まったかが分かる
#   etc/         inittab、init.d、rc.d など、rc.sh より前に走るスクリプト一式
#   mounts.txt mtd.txt cmdline.txt lsmod.txt version.txt printk.txt
#   fw_env.txt   u-boot の環境 (bootcmd、bootdelay、console)。読むだけ
#   device-tree/ /proc/device-tree の写し。ボード設定の記録用

ENTRY="$1"
NOW=$(cut -d' ' -f1 /proc/uptime)

ROOT=/mnt/extsd/boot-trace
( : > /mnt/extsd/.wtest ) 2>/dev/null || exit 0
rm -f /mnt/extsd/.wtest
mkdir -p "$ROOT" || exit 0
n=1
while [ -e "$ROOT/$(printf %03d $n)" ]; do
	n=$((n + 1))
done
D="$ROOT/$(printf %03d $n)"
mkdir -p "$D" || exit 0

{
	echo "entry $ENTRY"
	echo "trace_start $NOW"
	echo "clk_tck $(getconf CLK_TCK 2>/dev/null || echo unknown)"
} > "$D/uptime.txt"

# プロセスの開始時刻。/proc/<pid>/stat の 22 番目 (起動からの tick 数)。
# comm に空白が入ることがあるので ')' より後ろだけを数える。
{
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		stat=$(cat "$p/stat" 2>/dev/null) || continue
		rest=${stat##*) }
		set -- $rest
		ppid=$2
		ticks=${20}
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
		[ -n "$cmd" ] || cmd="[$(cat "$p/comm" 2>/dev/null)]"
		echo "$ticks $pid $ppid $cmd"
	done | sort -n
} > "$D/procs.txt" 2>&1

dmesg > "$D/dmesg.txt" 2>&1
cat /proc/mounts > "$D/mounts.txt" 2>&1
cat /proc/mtd > "$D/mtd.txt" 2>&1
cat /proc/cmdline > "$D/cmdline.txt" 2>&1
cat /proc/version > "$D/version.txt" 2>&1
cat /proc/sys/kernel/printk > "$D/printk.txt" 2>&1
lsmod > "$D/lsmod.txt" 2>&1
tr '\0' ' ' < /proc/1/cmdline > "$D/init-cmdline.txt" 2>&1
fw_printenv > "$D/fw_env.txt" 2>&1

mkdir -p "$D/etc"
for f in /etc/inittab /etc/fstab /etc/preinit /etc/rc.local /etc/rc.common /etc/profile; do
	[ -e "$f" ] && cp "$f" "$D/etc/" 2>/dev/null
done
for d in /etc/init.d /etc/rc.d; do
	[ -d "$d" ] && cp -r "$d" "$D/etc/" 2>/dev/null
done
ls -l /etc/init.d /etc/rc.d > "$D/etc/listing.txt" 2>&1
# rc.sh を誰が呼ぶか
grep -rl "rc.sh" /etc 2>/dev/null > "$D/etc/who-calls-rc.txt"

[ -d /proc/device-tree ] && cp -r /proc/device-tree "$D/device-tree" 2>/dev/null

echo "trace_end $(cut -d' ' -f1 /proc/uptime)" >> "$D/uptime.txt"
sync

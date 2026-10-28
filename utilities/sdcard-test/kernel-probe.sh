#!/bin/sh
# カーネルとブートローダを作り直せるかを判断するための情報を、カードの
# kernel-probe/ に集める (2026-10-19)。読むだけで、フラッシュにも設定にも
# 書かない。rc-exp.sh が記録の終わり (ビープの前) に呼ぶ。止めるときは
# kernel-probe.sh.off に改名する。数百 ms。
O=/mnt/extsd/kernel-probe
mkdir -p "$O" 2>/dev/null || exit 0
{
	echo "== stage $1 boot $2 uptime $(cut -d' ' -f1 /proc/uptime)"
	echo "== uname"; uname -a
	echo "== /proc/version"; cat /proc/version
	echo "== cmdline"; cat /proc/cmdline
	echo "== config.gz"; ls -l /proc/config.gz 2>&1
	echo "== ikconfig in kallsyms"; grep -c -i ikconfig /proc/kallsyms 2>&1
	echo "== printk"; cat /proc/sys/kernel/printk
	echo "== pstore"; grep -i pstore /proc/filesystems; ls -la /sys/fs/pstore 2>&1; ls /sys/module 2>/dev/null | grep -i -E 'ramoops|pstore'
	echo "== cpufreq governors"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors 2>&1
	echo "== modules"; cat /proc/modules
	echo "== vermagic of /mnt/app/ko"
	for k in /mnt/app/ko/*.ko; do
		printf "%s: " "${k##*/}"; tr '\000' '\n' < "$k" | grep -m1 '^vermagic=' || echo "(none)"
	done
	echo "== filesystems"; cat /proc/filesystems
	echo "== mtd"; cat /proc/mtd
	echo "== iomem"; cat /proc/iomem
	echo "== /sys/module (built-in and loaded)"; ls /sys/module | tr '\n' ' '; echo
	echo "== u-boot env (read only)"; fw_printenv 2>&1
} > "$O/info-stage$1.txt" 2>&1
[ -e /proc/config.gz ] && cp /proc/config.gz "$O/config.gz" 2>/dev/null
# カーネルの関数と変数の一覧 (約 2MB)。作り直したカーネルと組み込みの部品を
# 比べるため。一度写せば次からは写さない (2026-10-19)。
[ -s "$O/kallsyms.txt" ] || cat /proc/kallsyms > "$O/kallsyms.txt" 2>/dev/null
sync
exit 0

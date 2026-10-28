#!/bin/sh
# NOR の中身を読むだけで記録し、復旧イメージ (HDZG_OS.bin 9.5.1) の各項目の md5 と比べる
# (2026-10-20 夜、作ったカーネル hdzlike6 の起動で NOR が壊れた後の診断)。
# カードには kernel-probe.sh の名前で置く (rc-exp.sh が記録の終わりに "$STAGE" "$BOOTN" を付けて呼ぶ)。
# 書くのはカードの nor-check/ だけ。32 MB を読むので 10 秒ほど。
O=/mnt/extsd/nor-check
mkdir -p "$O" 2>/dev/null || exit 0
F="$O/nor-stage$1-boot$2.txt"
chk() { # $1 = 名前, $2 = mtdblock, $3 = バイト数, $4 = 期待の md5, $5 = 先頭を飛ばすバイト数 (省略 0)
	skip=${5:-0}
	if [ "$skip" = 0 ]; then got=$(head -c "$3" "$2" | md5sum | cut -d' ' -f1)
	else got=$(dd if="$2" bs=512 skip=$((skip / 512)) count=$(( ($3 + 511) / 512 )) 2>/dev/null | head -c "$3" | md5sum | cut -d' ' -f1); fi
	[ "$got" = "$4" ] && r=match || r=DIFF
	echo "$r $1 ($2 +$skip, $3 B): got $got expected $4"
}
{
	echo "== uptime $(cut -d' ' -f1 /proc/uptime)"
	echo "== cmdline: $(cat /proc/cmdline)"
	echo "== uname: $(uname -a)"
	echo "== /proc/mtd"; cat /proc/mtd
	echo "== mounts"; grep -E 'mtdblock|overlay|squashfs|rootfs' /proc/mounts
	echo "== compare with the recovery image"
	chk boot0_spinor   /dev/mtdblock0 49152   3ac039f3b2827597029db1b92496b615
	chk boot_package   /dev/mtdblock0 671744  d0b4b3e4d0c8e5a60681fb25003656a5 65536
	chk kernel_uImage  /dev/mtdblock1 2939296 1b65ffaf9edb157f3760e25362a2da32
	chk rootfs_squash  /dev/mtdblock2 7733248 31dae0e58e09e16fae4469c45dc47e87
	chk bootlogo       /dev/mtdblock5 86716   6bf83182acfb4190d7412627483ee328
	echo "== first 64 KB of the writable partitions (for the record)"
	for m in 3 4 6 7; do printf 'mtdblock%s: ' $m; head -c 65536 /dev/mtdblock$m | md5sum | cut -d' ' -f1; done
	echo "== u-boot env"; fw_printenv 2>&1 | head -40
	echo "== /mnt/app"; ls -la /mnt/app 2>&1 | head -20; md5sum /mnt/app/script/rc.sh /mnt/app/app/HDZGOGGLE 2>&1
	echo "== /overlay/upperdir/etc/init.d"; ls -la /overlay/upperdir/etc/init.d 2>&1; md5sum /overlay/upperdir/etc/init.d/* 2>&1
	echo "== dmesg (jffs2 / mtd / spi / squashfs)"; dmesg | grep -i -E 'jffs2|mtd|spi|m25p|squashfs|overlay' | tail -60
} > "$F" 2>&1
sync
exit 0

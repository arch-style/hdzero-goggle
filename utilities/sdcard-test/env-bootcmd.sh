#!/bin/sh
# カードに置いたカーネルを試すための起動手順の書き換えと、1 段ずつの試験
# (2026-10-19、ユーザーの了承済み)。rc-exp.sh が記録の終わりに呼ぶ。
# u-boot の bootcmd を、カードの hdztest.img があればそれを起動し (引数に
# hdzcard=1 を足す)、無ければ今までどおりフラッシュのカーネルを起動する形にする。
# 書くのは u-boot の環境変数 bootcmd だけ。書く前の値が期待どおりでないか、
# 書いた後に bootcmd 以外が変わっていたら止めて result.txt に書く。
# env-bootcmd/RESTORE を置くと、次の起動で元の bootcmd に戻す。
R=/mnt/extsd/env-bootcmd
E=/mnt/extsd
[ -d "$R" ] || exit 0
L="$R/result.txt"
say() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$L"; sync; }
ORIG='run hdzero_update setargs_nor boot_normal'
NEW='run hdzero_update; run setargs_nor; mw.l 45000000 0 40; fatload mmc 0:1 45000000 hdztest.img; setenv bootargs ${bootargs} hdzcard=1; bootm 45000000; run setargs_nor; run boot_normal'
[ -n "$(fw_printenv -n bootcmd 2>/dev/null)" ] || { say "stop: fw_printenv が読めない"; exit 0; }
cur=$(fw_printenv -n bootcmd 2>/dev/null)
card=no; grep -q 'hdzcard=1' /proc/cmdline && card=yes
ver=$(uname -v)
say "boot: card=$card ver=[$ver] test_file=$([ -e $E/hdztest.img ] && md5sum < $E/hdztest.img | cut -c1-8 || echo none) modules=$(wc -l < /proc/modules) bootcmd=$([ "$cur" = "$NEW" ] && echo NEW || ([ "$cur" = "$ORIG" ] && echo ORIG || echo OTHER))"

setv() { # $1 = want, $2 = tag
	fw_printenv > "$R/before-$2.txt" 2>&1
	fw_setenv bootcmd "$1"
	fw_printenv > "$R/after-$2.txt" 2>&1
	got=$(fw_printenv -n bootcmd 2>/dev/null)
	b=$(grep -v '^bootcmd=' "$R/before-$2.txt" | md5sum | cut -d' ' -f1)
	a=$(grep -v '^bootcmd=' "$R/after-$2.txt" | md5sum | cut -d' ' -f1)
	n=$(grep -c . "$R/after-$2.txt")
	if [ "$got" = "$1" ] && [ "$a" = "$b" ] && [ "$n" -gt 10 ]; then
		say "$2: wrote bootcmd, nothing else changed"; date > "$R/DONE-$2"; sync
	else
		say "$2: CHECK FAILED got=[$got] rest_same=$([ "$a" = "$b" ] && echo yes || echo no) lines=$n"
	fi
}
place() { # $1 = staged file name on the card
	if [ -f "$E/$1" ] && cp "$E/$1" "$E/hdztest.tmp" && [ "$(md5sum < "$E/$1")" = "$(md5sum < "$E/hdztest.tmp")" ]; then
		mv "$E/hdztest.tmp" "$E/hdztest.img"; sync; say "placed $1 as hdztest.img"
	else
		rm -f "$E/hdztest.tmp"; sync; say "could not place $1"; echo stop > "$R/step"
	fi
}
off() { [ -e "$E/hdztest.img" ] && mv "$E/hdztest.img" "$E/hdztest.img.off"; sync; }

if [ -e "$R/RESTORE" ]; then
	off
	if [ "$cur" = "$NEW" ]; then setv "$ORIG" restore; elif [ "$cur" = "$ORIG" ]; then say "restore: already ORIG"; fi
	exit 0
fi
if [ ! -e "$R/DONE-write" ]; then
	off
	if [ "$cur" = "$ORIG" ]; then setv "$NEW" write
	elif [ "$cur" = "$NEW" ]; then say "write: already NEW"; date > "$R/DONE-write"
	else say "stop: bootcmd is [$cur], expected the stock one"; fi
	echo 1 > "$R/step"; sync
	exit 0
fi
[ "$cur" = "$NEW" ] || { say "stop: bootcmd is not NEW"; exit 0; }
case "$(cat "$R/step" 2>/dev/null)" in
1)	if [ "$card" = no ] && [ ! -e "$E/hdztest.img" ]; then
		say "step1 ok: no card kernel, booted from flash under the new bootcmd"; place hdz-stock69.img; [ -e "$E/hdztest.img" ] && echo 2 > "$R/step"
	else say "step1 unexpected"; off; echo stop > "$R/step"; fi ;;
2)	if [ "$card" = yes ] && echo "$ver" | grep -q '#69 '; then
		say "step2 ok: stock #69 kernel booted from the card"; place hdz-like4.img; [ -e "$E/hdztest.img" ] && echo 3 > "$R/step"
	else say "step2 failed: card=$card"; off; echo stop > "$R/step"; fi ;;
3)	if [ "$card" = yes ] && ! echo "$ver" | grep -q '#69 '; then
		say "step3 ok: rebuilt kernel booted from the card"
		cat /proc/modules > "$R/modules-like4.txt"; dmesg > "$R/dmesg-like4.txt" 2>/dev/null; uname -a > "$R/uname-like4.txt"
	else say "step3 failed: card=$card"; fi
	off; echo 4 > "$R/step" ;;
*)	: ;;
esac
sync
exit 0

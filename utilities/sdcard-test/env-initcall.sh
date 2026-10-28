#!/bin/sh
# カーネルの各ドライバの初期化の時間を一度だけ測る (2026-10-19、ユーザーの了承済み)。
# u-boot の環境変数 initcall_debug を "0" から "1 log_buf_len=1M" に変え、次の起動で
# 記録を取ったら、その起動の中で "0" に戻す。rc-exp.sh が記録の終わりに呼ぶ。
#   起動 1: 0 を確かめて書く (DONE-on)
#   起動 2: カーネルが initcall_debug=1 で動いていたら dmesg を写し、0 に戻す (DONE-off)
#   起動 3 以降: 何もしない
# 書く前の値が期待どおりでない、または書いた後に initcall_debug 以外が変わっていたら、
# 止めて result.txt に書く。環境変数以外 (フラッシュの他の領域) には書かない。
R=/mnt/extsd/env-initcall
[ -d "$R" ] || exit 0
[ -e "$R/DONE-off" ] && exit 0
L="$R/result.txt"
say() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$L"; sync; }
ON="1 log_buf_len=1M"
OFF="0"
cur=$(fw_printenv -n initcall_debug 2>/dev/null)
[ -n "$(fw_printenv -n bootcmd 2>/dev/null)" ] || { say "stop: fw_printenv が読めない"; exit 0; }
cat /etc/fw_env.config > "$R/fw_env.config.txt" 2>&1

setv() { # $1 = want, $2 = tag
	fw_printenv > "$R/before-$2.txt" 2>&1
	fw_setenv initcall_debug "$1"
	fw_printenv > "$R/after-$2.txt" 2>&1
	got=$(fw_printenv -n initcall_debug 2>/dev/null)
	# initcall_debug の行を除いた残りが、書く前と同じであること (md5 で比べる)
	b=$(grep -v '^initcall_debug=' "$R/before-$2.txt" | md5sum | cut -d' ' -f1)
	a=$(grep -v '^initcall_debug=' "$R/after-$2.txt" | md5sum | cut -d' ' -f1)
	n=$(grep -c . "$R/after-$2.txt")
	if [ "$got" = "$1" ] && [ "$a" = "$b" ] && [ "$n" -gt 10 ]; then
		say "$2: wrote initcall_debug=[$1], nothing else changed"; date > "$R/DONE-$2"; sync
	else
		say "$2: CHECK FAILED got=[$got] rest_same=$([ "$a" = "$b" ] && echo yes || echo no) lines=$n (see before/after-$2.txt)"
	fi
}

if [ ! -e "$R/DONE-on" ]; then
	if [ "$cur" = "$OFF" ]; then setv "$ON" on
	elif [ "$cur" = "$ON" ]; then say "on: already [$ON]"; date > "$R/DONE-on"
	else say "stop: initcall_debug is [$cur], expected [$OFF]"; fi
	exit 0
fi
# 測る起動: カーネルがその指定で動いていたか
if grep -q 'initcall_debug=1 log_buf_len=1M' /proc/cmdline; then
	dmesg > "$R/dmesg-initcall.txt" 2>/dev/null
	cp /proc/cmdline "$R/cmdline.txt"
	say "measured: $(grep -c 'initcall ' "$R/dmesg-initcall.txt") initcall lines"
	if [ "$cur" = "$ON" ]; then setv "$OFF" off; else say "stop: initcall_debug is [$cur], expected [$ON]"; fi
else
	say "this boot's cmdline lacks the setting; waiting (cmdline: $(cat /proc/cmdline | cut -c1-60))"
fi
exit 0

#!/bin/sh
# Mac から Goggle 2 に SSH で入れるように、ゴーグルの方から Mac へ ping を出す。
# develop.sh (develop.sh.dual) の Goggle 2 側から裏で起動する。
#
# 2026-10-16 の調べでは、Wi-Fi (子機モード) はつながって IP も持っているのに、
# Mac からの呼びかけ (ARP の一斉送信) にゴーグルが応えず、Mac からは見え
# なかった。ゴーグルが Mac へ一度通信すると、その時点で Mac がゴーグルの
# 機器番号を覚え、以後は SSH も ping も通った (wifi-state.sh で確認)。機器番号は
# 起動のたびに変わるので、起動ごとに必要。
#
# 宛先はカードの wifi-hello.txt に 1 行 1 つ (複数可)。無ければ何もしない。
# Mac の IP は場所や時間で変わる (macOS の「プライベート Wi-Fi アドレス」が
# 回ると IP も変わる) ので、行に sweep と書くと、wlan0 と同じ /24 の全部の
# 番地に 1 回ずつ ping を出して、どの IP の Mac にも覚えてもらう (約 1〜2 分)。
# Wi-Fi が IP を持つまでは 2 秒ごとに見て、持ってからは 30 秒ごとに宛先へ
# ping を 1 回ずつ送る (Mac の覚えを切らさないため)。メニューで Wi-Fi を
# オフにしたままなら ping は送らない。何も書かない。
# このゴーグルの busybox の ping は引数を取らず、1 回だけ送って返事を 5 秒まで
# 待つ。

F=/mnt/extsd/wifi-hello.txt
[ -f "$F" ] || exit 0
TARGETS=$(tr -d ' \r' < "$F" | grep -v '^$' | grep -v '^sweep$')
SWEEP=$(tr -d ' \r' < "$F" | grep -c '^sweep$')
[ -n "$TARGETS" ] || [ "$SWEEP" != 0 ] || exit 0

sweep() {
	net=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9]*\.[0-9]*\.[0-9]*\)\.[0-9]*.*/\1/p')
	[ -n "$net" ] || return
	n=1
	while [ $n -le 254 ]; do
		m=0
		while [ $m -lt 16 ] && [ $n -le 254 ]; do
			ping "$net.$n" > /dev/null 2>&1 &
			n=$((n + 1)); m=$((m + 1))
		done
		wait
	done
}

swept=0
i=0
while [ $i -lt 2000 ]; do
	if ifconfig wlan0 2>/dev/null | grep -q 'inet addr'; then
		for T in $TARGETS; do
			ping "$T" > /dev/null 2>&1 &
		done
		if [ "$SWEEP" != 0 ] && [ $swept = 0 ]; then
			swept=1
			sweep
		fi
		wait
		sleep 30
	else
		sleep 2
	fi
	i=$((i + 1))
done

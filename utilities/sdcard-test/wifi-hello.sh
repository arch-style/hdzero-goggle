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
# 宛先の Mac の IP はカードの wifi-hello.txt に 1 行で書く。無ければ何もしない。
# Wi-Fi が IP を持つまでは 2 秒ごとに見て、持ってからは 30 秒ごとに ping を 1 回
# 送る (Mac の覚えを切らさないため)。メニューで Wi-Fi をオフにしたままなら
# ping は送らない。何も書かない。

T=$(head -1 /mnt/extsd/wifi-hello.txt 2>/dev/null | tr -d ' \r')
[ -n "$T" ] || exit 0

i=0
while [ $i -lt 2000 ]; do
	if ifconfig wlan0 2>/dev/null | grep -q 'inet addr'; then
		ping "$T" > /dev/null 2>&1
		sleep 30
	else
		sleep 2
	fi
	i=$((i + 1))
done

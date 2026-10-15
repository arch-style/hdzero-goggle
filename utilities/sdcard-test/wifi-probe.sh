#!/bin/sh
# Goggle 2 の Wi-Fi が何を見ているか、自宅のネットワークにつながるかを調べる。
# autotest.wifi.txt から段階ごとに呼び、出力は HDZGOGGLE.log の "AT shell" 行に
# 出る。Wi-Fi はメニューでオフのまま使う (アプリの Wi-Fi と取り合わないように)。
#
#   start   ドライバを入れ、wpa_supplicant を起動する
#   scan    周りのネットワーク (SSID、周波数、強さ、暗号) を一覧にする
#   status  設定の SSID に関連付けできたか (wpa_state=COMPLETED なら成功)
#   log     wpa_supplicant の経過から、関連付けと認証の行だけを出す
#   stop    全部止めてドライバを外す。メニューで Wi-Fi を切ったときと同じ状態
#
# フラッシュには書かない。設定 (SSID とパスワード) はアプリが起動時に
# /tmp/wpa_supplicant.conf へ書き出したものを読み、制御用のソケットと経過は
# /tmp に置く。パスワードはログに出さない。DHCP (udhcpc) は走らせない。
# 走らせると /etc/resolv.conf を書き換え、それがフラッシュの上書き領域に入る。

# 1 回目 (2026-10-16) は wpa_supplicant を起動した直後に、アプリのログが
# どのスレッドの分も止まった。どこで止まるかを見るため、段階ごとの印を
# カードの wifi-probe.marks に書いて sync する。ログと印の両方が止まるなら
# カードへの書き込みごと止まっている。busybox の sh には command が無いので
# 道具の有無は which で見る。wpa_supplicant は純正の wlan_start_sta.sh と
# 同じ引数で起動する (-d は付けない)。
MARKS=/mnt/extsd/wifi-probe.marks
mark() {
	echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$MARKS"
	sync
}

CONF=/tmp/probe_wpa.conf
CTRL=/tmp/probe_wpa
WLOG=/tmp/probe_wpa.log

cli() {
	wpa_cli -p "$CTRL" -i wlan0 "$@"
}

case "$1" in
start)
	for t in wpa_supplicant wpa_cli iw iwlist; do
		echo "tool $t: $(which $t || echo none)"
	done
	mark "start"
	echo "lsmod before: $(lsmod | grep -c xradio) xradio modules"
	insmod /mnt/app/ko/xradio_mac.ko
	insmod /mnt/app/ko/xradio_core.ko
	insmod /mnt/app/ko/xradio_wlan.ko
	mark "insmod done"
	sleep 2
	ifconfig wlan0 up
	mark "wlan0 up"
	ifconfig wlan0 | head -2
	if [ ! -f /tmp/wpa_supplicant.conf ]; then
		echo "no /tmp/wpa_supplicant.conf (アプリが書いていない)"
		exit 0
	fi
	# アプリの設定から network ブロックだけを使い、置き場所を /tmp にする。
	{
		echo "ctrl_interface=$CTRL"
		echo "update_config=0"
		sed -n '/^network={/,/^}/p' /tmp/wpa_supplicant.conf
	} > "$CONF"
	echo "ssid in config: $(grep '^ssid=' "$CONF")"
	echo "psk length: $(sed -n 's/^psk="\(.*\)"$/\1/p' "$CONF" | tr -d '\n' | wc -c)"
	mark "wpa_supplicant launch"
	wpa_supplicant -Dnl80211 -iwlan0 -c"$CONF" > "$WLOG" 2>&1 &
	for i in 1 2 3 4 5 6 7 8 9 10; do
		sleep 1
		mark "after launch ${i}s pid=$(pidof wpa_supplicant)"
	done
	echo "wpa_supplicant pid: $(pidof wpa_supplicant)"
	;;
scan)
	mark "scan"
	if which wpa_cli > /dev/null; then
		cli scan
		sleep 6
		cli scan_results
	elif which iw > /dev/null; then
		iw dev wlan0 scan | grep -E '^BSS|freq:|signal:|SSID:|RSN|WPA'
	else
		iwlist wlan0 scan | grep -E 'Cell|Frequency|Quality|ESSID|IE:'
	fi
	;;
status)
	mark "status"
	if which wpa_cli > /dev/null; then
		cli status | grep -v '^passphrase\|^psk'
	else
		iw dev wlan0 link 2>&1
	fi
	;;
log)
	grep -E 'CTRL-EVENT|Trying to associate|Associated with|WPA: Key negotiation|4-Way|WRONG_KEY|reason=|No suitable network|skip|ssid_mismatch|wpa_ie|freq=|Authentication with|SME:' "$WLOG" \
		| grep -v -i 'psk\|passphrase' | tail -120
	;;
stop)
	killall wpa_supplicant 2>/dev/null
	sleep 1
	ifconfig wlan0 down 2>/dev/null
	rmmod xradio_wlan.ko 2>/dev/null
	rmmod xradio_core.ko 2>/dev/null
	rmmod xradio_mac.ko 2>/dev/null
	rm -rf "$CTRL" "$CONF"
	echo "lsmod after: $(lsmod | grep -c xradio) xradio modules"
	mark "stopped"
	;;
esac

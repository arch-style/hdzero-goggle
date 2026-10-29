#!/bin/sh
# メニューの Wi-Fi (子機モード) が動いているときの状態を、読むだけで出す。
# autotest.wifistate.txt から一定間隔で呼ぶ。出力は HDZGOGGLE.log の
# "AT shell" 行。2026-10-16、Wi-Fi は起動時に立ち上がり SSH も起動したのに、
# Mac からはゴーグルが見えなかった。どこで止まっているかを見る:
# 関連付け (/proc/net/wireless, dmesg)、IP (ifconfig)、経路、ルーターへの ping。
# 何も変えない。ping はルーターと Mac に 1 回ずつ送るだけ。
# busybox の ping はオプションを受け付けない版 ("ping 宛先" で 1 回送って
# "is alive" か応答なしを出す)。
#
#   (引数なし)  状態を 1 回出す
#   keepalive   Mac へ 2 秒ごとに ping を 90 秒間、裏で送り続ける。ゴーグルから
#               通信を出し続けたときに Mac から見えるようになるかを見る
# Mac の IP はカードの wifi-hello.txt の 1 行目 (wifi-hello.sh と同じ)。無ければ
# 以前の自宅の値。
# カードの整理 (2026-10-29): 動かすのに要るものは fork-system/、消してよいもの
# (ログ、古い版) は fork-deletable/。カード直下に残すのは純正のもの
# (DCIM, FIRMWARE, FSCK, log, HDZG_*.bin などの決まった名前) と、フラッシュの
# 入口が名前で呼ぶ rc-exp.sh, init-exp.sh, early-exp.sh, develop.sh だけ。
SYS=/mnt/extsd/fork-system
DEL=/mnt/extsd/fork-deletable
MAC_IP=$(head -1 $SYS/wifi-hello.txt 2>/dev/null | tr -d ' \r')
[ -n "$MAC_IP" ] || MAC_IP=192.168.40.46

if [ "$1" = keepalive ]; then
	(
		i=0
		while [ $i -lt 45 ]; do
			ping "$MAC_IP" > /dev/null 2>&1
			sleep 2
			i=$((i + 1))
		done
	) > /dev/null 2>&1 &
	echo "keepalive started"
	exit 0
fi

echo "uptime $(cut -d' ' -f1 /proc/uptime)"
echo "pids wpa=$(pidof wpa_supplicant) dhcp=$(pidof udhcpc) ssh=$(pidof dropbear) rtsp=$(pidof rtspLive)"
ifconfig wlan0 2>&1 | grep -E 'HWaddr|inet addr|UP|RX packets|TX packets'
grep wlan0 /proc/net/wireless 2>&1
route -n 2>&1 | grep -v '^Kernel'
GW=$(route -n 2>/dev/null | awk '$1 == "0.0.0.0" {print $2; exit}')
if [ -n "$GW" ]; then
	echo "ping gw: $(ping "$GW" 2>&1 | tail -1)"
fi
echo "ping mac: $(ping "$MAC_IP" 2>&1 | tail -1)"
arp -n 2>/dev/null || cat /proc/net/arp
# 省電力の設定が見える場所があれば出す (読むだけ)。
for f in /sys/module/xradio_wlan/parameters/* /sys/module/xradio_core/parameters/*; do
	[ -r "$f" ] && echo "param ${f#/sys/module/}=$(cat "$f" 2>/dev/null)"
done
ls /sys/kernel/debug/ieee80211/*/netdev:wlan0 2>/dev/null | tr '\n' ' '; echo
dmesg 2>/dev/null | grep -i -E 'deauth|disassoc|reason|power|ps_|beacon|lost|assoc|auth|xradio.*(join|connect|scan)' | tail -12
# 子機モードの設定 (アプリが設定から作る)。鍵は伏せる。
sed 's/psk=.*/psk=<hidden>/' /tmp/wpa_supplicant.conf 2>/dev/null
# wpa_supplicant の制御ソケットがあれば、名前だけ (wpa_cli は無い)。
ls /var/log/wpa_supplicant 2>/dev/null | tr '\n' ' '; echo
ps 2>/dev/null | grep -E 'wpa_supplicant|udhcpc|dropbear|wifi-hello' | grep -v grep

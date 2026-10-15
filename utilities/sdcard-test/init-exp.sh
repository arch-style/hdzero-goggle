#!/bin/sh
# Goggle 2 の起動時間の実験: OS の初期化スクリプト (/etc/init.d/S??*) の扱いを
# 決める。カードの直下に置くと、上書き領域の rc.final に足した入口
# (2026-10-16) が、カードがマウントされた後 (S01app より後) の S スクリプト
# ごとに、そのパスを引数にしてこれを呼ぶ。
#
#   0 で終わる      その S スクリプトは済んだもの (ここで走らせた、裏に回した、
#                   飛ばした) として、rc.final は次へ進む
#   0 以外で終わる  rc.final が純正どおり走らせる
#
# S00mpp、S00part、S01app はカードのマウント前なので、ここには来ない。
# 消すかカードを抜けば純正どおり。各 S スクリプトの開始と終了の経過秒を
# カードの init-exp.log に書く。

# --- 実験の設定 (S スクリプトの名前ごとに run / bg / skip) ---
#   run   ここで前と同じに走らせ、時間を測る (純正と同じ動き)
#   bg    裏で走らせて、終わりを待たずに次へ進む
#   skip  走らせない
# 実験 i1 (全部 run) の時間: S10udev 2.27 秒、S50usb 2.14 秒、ほかは合わせて
# 0.1 秒。カードのマウントは 3.04 秒までに済み、rc.sh は 7.61 秒に始まった。
# 実験 i2: S50usb を飛ばす (ADB とカメラ機能の USB の準備。Goggle 2 には USB の
# 口が無い)。S10udev は裏に回す (SD カードの抜き差しでのマウントを担うので
# 止めない。完了を待たないだけ)。
# 実験 i2 の結果: 起動ロゴのまま止まった。rc.sh が 4.14 秒に始まり、4.29 秒の
# ドライバの読み込み (mcp3021, nct75) が 14.57 秒まで 10 秒かかり、アプリも
# I2C port 1 (BMI270 0x68, IT66121 0x4C) の読み書きが毎回 0.5 秒の時間切れで
# 先へ進まなかった。i1 では rc.sh が 7.61 秒に始まり問題なし。早すぎる I2C か、
# 裏に回した udev との重なりのどちらかが原因。
# 実験 i3: udev は今までどおり待ち、S50usb だけ飛ばす (rc.sh は 5.4 秒ごろ)。
# 結果: rc.sh 6.10 秒、ドライバの読み込みが 6.15-16.25 秒で、やはり起動画面で
# 止まった。udev の重なりではない。i4: 同じ設定のまま、rc-exp.sh が I2C に
# 触る前に 7.6 秒まで待つ (I2C_NOT_BEFORE)。
mode_for() {
	case "$1" in
	S10udev)     echo run ;;
	S11dev)      echo run ;;
	S40network)  echo run ;;
	S41netparam) echo run ;;
	S50usb)      echo skip ;;
	*)           echo stock ;;
	esac
}

LOG=/mnt/extsd/init-exp.log
t() {
	echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$LOG" 2>/dev/null
}

# 実験 i6: rc-exp.sh の PRELOAD (i5) を、カードがマウントされた直後 (最初に
# 呼ばれる S10udev の時点、3.7 秒ごろ) に前倒しする。i5 では rc.sh の開始
# (6.2 秒) から読み始め、8.39 秒に読み終わって、アプリの起動 (8.30 秒) に
# わずかに間に合わなかった。rc-exp.sh の PRELOAD は 0 にしておくこと。
EARLY_PRELOAD=1
# 実験 C1: 画面の設定 (rc.sh の setup_uart と dispw、約 1.3 秒) も同じ時点で
# 裏で始める。アプリを I2C の関門より前に起動するので、アプリの lvgl_init より
# 前に画面の設定が終わっている必要がある (実験 2)。終わったら
# /tmp/dispw.done を作り、rc-exp.sh はそれを待つ。dispw は I2C を使わない
# (i2 では 4.1 秒に走って普通に終わった)。
EARLY_DISPW=1

name=${1##*/}
if [ "$EARLY_PRELOAD" = 1 ] && [ "$name" = S10udev ] && [ -f /mnt/extsd/preload.txt ]; then
	t "preload start"
	( while read -r f; do cat "$f" > /dev/null 2>&1; done < /mnt/extsd/preload.txt
	  t "preload done" ) &
fi
if [ "$EARLY_DISPW" = 1 ] && [ "$name" = S10udev ]; then
	: > /tmp/dispw.started
	t "dispw start (early)"
	( echo 0x0300B098 0x00775577 > /sys/class/sunxi_dump/write
	  echo 0x0300B0D8 0x22777777 > /sys/class/sunxi_dump/write
	  echo 0x0300B0fc 0x35517751 > /sys/class/sunxi_dump/write
	  dispw -x > /dev/null 2>&1
	  dispw -s vdpo 1080p50 > /dev/null 2>&1
	  : > /tmp/dispw.done
	  t "dispw done (early)" ) &
fi
mode=$(mode_for "$name")
case "$mode" in
run)
	t "$name start"
	"$1" start > /dev/null 2>&1
	t "$name end"
	exit 0
	;;
bg)
	t "$name background"
	( "$1" start > /dev/null 2>&1; t "$name end (background)" ) &
	exit 0
	;;
skip)
	t "$name skipped"
	exit 0
	;;
*)
	t "$name stock"
	exit 1
	;;
esac

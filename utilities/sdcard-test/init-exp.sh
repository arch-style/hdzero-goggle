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
# 実験の段階 (stage): カードの stage.txt に数字があれば、その番号の設定で起動し、
# 番号を 1 増やして書き戻す (次の起動は次の段階)。カードを往復させずに
# 続けて 2 通り試すため。無ければ 0 (下の既定)。rc-exp.sh は /tmp/exp-stage で
# 同じ番号を知る。
#   1  S10udev は udevd だけ起動 (coldplug の trigger/settle をやめる、約 2.3 秒)。
#      アプリの I2C は全 port を関門 (7.6 秒) まで待たせる (安全側)。
#      3.8〜9 秒の I2C のピンの電位を pins.log に記録 (読むだけ)。
#   2  同じく udevd だけ。アプリの I2C は port 1 だけ関門 (port 2/3 は 4.3 秒
#      ごろから。port 2 が早くから使えるかの確認)。
#   3  udevd だけ + /dev/rtc のリンクを作る (段階 1/2 では udev の規則が走らず
#      /dev/rtc が無くて fork が落ちた)。port 1 だけ関門。/dev の一覧を控える。
#   4  段階 3 と同じで、先読み (EARLY_PRELOAD) を止める。先読みとアプリの起動が
#      NOR の読み込みを取り合うので、どちらが速いかを見る。
#   5  純正どおり S10udev を走らせる (比較用の基準。/dev の一覧の差分を取る)。
#   6  段階 3 と同じ (udevd + /dev/rtc、先読みあり)。アプリのログを残す版。
#   7  段階 4 と同じ (udevd + /dev/rtc、先読みなし)。
#   8  段階 6 と同じ (ばらつきを見る)。
#   9  udevd + /dev/rtc。NOR の先読みの代わりに、カードの g2lib/ (アプリと録画
#      プロセスが使う共有ライブラリ 47 本、7.1MB、純正の rootfs の写し) を
#      /tmp/lib に写す (SD は NOR の数倍速い)。rc-exp.sh が LD_LIBRARY_PATH の
#      先頭に /tmp/lib を置くので、アプリはそこから読む。新しい fork 本体
#      (時計が読めなくても落ちない、HDZ_DEFER_PORT1 対応) を既定の動きで。
#   10 段階 9 + HDZ_DEFER_PORT1=1 (案 E: HDMI 出力と IMU の port 1 を映像の後へ)。
#   11 段階 10 と同じ (ばらつき)。
#   12 段階 9 と同じ (ばらつき)。
#   13 段階 10 (udevd + rtc + ライブラリはカードから + 案 E) を既定にし、fork
#      本体 3.3MB もここで SD から /tmp に写しておく (develop.g2.sh の cp を省く)。
#   14 段階 13 + HDZ_OSD_BEFORE_TUNER=1 (OSD の組み立てをチューナー待ちの前に)。
#   15 段階 14 + HDZ_EFUSE_CACHE=1 (チューナーの efuse をカードに控える。初回は
#      読んで書くだけ)。
#   16 段階 15 と同じ (控えが効く 2 回目)。
#   17 段階 16 を既定にし、起動の経路にある sync (model-history、設定の控え) を
#      裏に回す (develop.sh.dual と goggle2-backup.sh が /tmp/exp-stage を見る)。
#   18 段階 17 + udevd の起動を待たない (裏で起動。S10udev が 0.3〜0.8 秒)。
#   19 段階 18 + S40network (ifup と udhcpc) を裏に回す。
#   20 段階 19 と同じ (ばらつき)。
#   21〜23 段階 20 + rc-exp.sh から fork を直接起動する (FAST_LAUNCH。develop.sh
#      の連鎖を通らない)。3 回同じ。
#   24 以降 段階 21 と同じ。判定は 999 まで同じ設定になる (2026-10-16 夜、
#      29 の次で純正寄りの起動に戻ってしまうのに気付いて広げた)。
# 段階 3/4 (2026-10-16) は落ちずに起動したが、アプリのログが次の起動で
# 上書きされて映像の時刻が読めなかった。段階 6 からは rc-exp.sh が記録の
# 終わりにログを boot-logs/ へ写す。/dev の差分 (段階 5 と 3) は udev の規則が
# 作るリンクだけだった: /dev/rtc のほかは disk/by-*、snd/by-path、block/、
# char/ で、どれも使っていない。
STAGE=0
if [ "${1##*/}" = S10udev ]; then
	n=$(tr -dc 0-9 < /mnt/extsd/stage.txt 2>/dev/null)
	if [ -n "$n" ]; then
		STAGE=$n
		echo $((n + 1)) > /mnt/extsd/stage.txt
		echo "$STAGE" > /tmp/exp-stage
	fi
else
	STAGE=$(cat /tmp/exp-stage 2>/dev/null)
	[ -n "$STAGE" ] || STAGE=0
fi
mode_for() {
	case "$1" in
	S10udev)     case "$STAGE" in 1|2|3|4|6|7|8|9|1[0-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) echo udevd ;; *) echo run ;; esac ;;
	S11dev)      echo run ;;
	S40network)  case "$STAGE" in 19|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) echo bg ;; *) echo run ;; esac ;;
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
case "$STAGE" in 4|7|9|1[0-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) EARLY_PRELOAD=0 ;; esac
# 段階 9 以降: 共有ライブラリをカードから /tmp/lib へ写す (下の EARLY_LIBCOPY)。
EARLY_LIBCOPY=0
case "$STAGE" in 9|1[0-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) EARLY_LIBCOPY=1 ;; esac
# 段階 13 以降: fork 本体もここで /tmp に写す。develop.g2.sh は
# /tmp/HDZGOGGLE.staged.done があり大きさが合えば、自分では cp しない。
EARLY_APPCOPY=0
case "$STAGE" in 1[3-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) EARLY_APPCOPY=1 ;; esac
# 段階 18 以降: udevd の起動を待たない (自分でデーモンになるまでの時間を待たず、
# 次の S スクリプトへ進む)。
UDEVD_BG=0
case "$STAGE" in 1[8-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) UDEVD_BG=1 ;; esac
# 実験 C1: 画面の設定 (rc.sh の setup_uart と dispw、約 1.3 秒) も同じ時点で
# 裏で始める。アプリを I2C の関門より前に起動するので、アプリの lvgl_init より
# 前に画面の設定が終わっている必要がある (実験 2)。終わったら
# /tmp/dispw.done を作り、rc-exp.sh はそれを待つ。dispw は I2C を使わない
# (i2 では 4.1 秒に走って普通に終わった)。
EARLY_DISPW=1

# dispw -x の代わり (rc-exp.sh の dispw_x と同じ)。カードの ko/disp.ko (1 秒待ちを
# 100 ms にした写し) があればそれを insmod し、無ければ純正の dispw -x。
dispw_x() {
	if [ -f /mnt/extsd/ko/disp.ko ]; then
		aww 0x07000160 0x083f10b7
		if insmod /mnt/extsd/ko/disp.ko; then
			t "disp.ko from card"
		else
			insmod /mnt/app/ko/disp.ko
			t "disp.ko from card FAILED, stock loaded"
		fi
		mount -t debugfs none /sys/kernel/debug
		insmod /mnt/app/ko/vdpo.ko
	else
		dispw -x
	fi
}

name=${1##*/}
# 最初に呼ばれる時点 (S10udev、約 3 秒) の dmesg をカードに控える。カーネルと
# 初期化の前半の内訳はここにしか残らない (後で押し出される)。数 ms。
# 起動ごとの写しも boot-logs/early-stage<段階>.txt に残す (2026-10-17。それまでは
# 毎回上書きで、最後の起動の分しか読めなかった)。
if [ "$name" = S10udev ]; then
	dmesg > /mnt/extsd/dmesg-early.txt 2>/dev/null
	mkdir -p /mnt/extsd/boot-logs 2>/dev/null
	cp /mnt/extsd/dmesg-early.txt "/mnt/extsd/boot-logs/early-stage$STAGE.txt" 2>/dev/null
	t "stage $STAGE"
	# CPU のクロックの履歴 (2026-10-17)。起動直後は 816 MHz で、約 2 秒ごとに
	# 1 段ずつ上がり、1104 MHz に届くのは 8 秒ほど後だった。どのクロックで
	# 何秒動いたか (単位 10 ms) を同じファイルに足す。rc-exp.sh も記録の終わりに足す。
	C=/sys/devices/system/cpu/cpu0/cpufreq
	{ echo "== cpufreq at S10udev $(cut -d' ' -f1 /proc/uptime)"
	  echo "gov=$(cat $C/scaling_governor) cur=$(cat $C/scaling_cur_freq) min=$(cat $C/scaling_min_freq) trans=$(cat $C/stats/total_trans)"
	  cat $C/stats/time_in_state; } >> "/mnt/extsd/boot-logs/early-stage$STAGE.txt" 2>/dev/null
	# ここから最低クロックを最高に固定する。ondemand は負荷を 2 秒に 1 回しか
	# 見ず (sampling_rate 2000000)、起動中は 816 MHz のまま待つ回が多い。段階
	# 47-53 の比べ (偶数だけ固定) で映像が 0.3-0.5 秒早かったので、段階 54 から
	# 全回に入れる。rc-exp.sh が記録の終わりに元の最低クロックへ戻す。電源を
	# 切れば元に戻る (フラッシュには書かない)。
	case "$STAGE" in *[02468]|5[4-9]|[6-9][0-9]|[1-9][0-9][0-9])
		cat $C/scaling_max_freq > $C/scaling_min_freq 2>/dev/null \
			&& t "cpu min set to $(cat $C/scaling_min_freq)" ;;
	esac
fi
# 段階 1/2: I2C のピンの電位を 9 秒まで 100ms ごとに記録する (読むだけ)。
# port 1 = twi1 = PE16/PE17 (PE_DATA 0x0300B0A0 の bit16/17)、
# port 2 = twi2 = PH9/PH10 (PH_DATA 0x0300B10C の bit9/10)。関門 (7.6 秒) より
# 前に線が低いままなら、部品側がバスを握っている。
if [ "$name" = S10udev ] && { [ "$STAGE" = 1 ] || [ "$STAGE" = 2 ]; }; then
	( P=/mnt/extsd/pins.log
	  echo "--- boot stage $STAGE: uptime PE_DATA PH_DATA" >> "$P"
	  while :; do
		u=$(cut -d' ' -f1 /proc/uptime)
		echo "$u $(awr 0x0300B0A0 2>&1 | tr '\n' ' ') $(awr 0x0300B10C 2>&1 | tr '\n' ' ')" >> "$P"
		case "$u" in 9.*|[1-9][0-9].*) break ;; esac
		usleep 100000 2>/dev/null || sleep 1
	  done
	  sync ) &
fi
if [ "$EARLY_APPCOPY" = 1 ] && [ "$name" = S10udev ] && [ -f /mnt/extsd/HDZGOGGLE ]; then
	( rm -f /tmp/HDZGOGGLE.staged.done
	  if cp /mnt/extsd/HDZGOGGLE /tmp/HDZGOGGLE.staged && chmod +x /tmp/HDZGOGGLE.staged; then
		stat -c %s /mnt/extsd/HDZGOGGLE > /tmp/HDZGOGGLE.staged.done 2>/dev/null || ls -l /mnt/extsd/HDZGOGGLE | awk '{print $5}' > /tmp/HDZGOGGLE.staged.done
		t "appcopy done"
	  fi ) &
fi
if [ "$EARLY_LIBCOPY" = 1 ] && [ "$name" = S10udev ] && [ -d /mnt/extsd/g2lib ]; then
	t "libcopy start"
	( mkdir -p /tmp/lib
	  cp /mnt/extsd/g2lib/*.so* /tmp/lib/ 2>/dev/null
	  # FAT にはシンボリックリンクが置けないので、soname の名前はここで作る。
	  while read -r link target; do
		[ -n "$link" ] && [ -f "/tmp/lib/$target" ] && ln -sf "$target" "/tmp/lib/$link"
	  done < /mnt/extsd/g2lib/links.txt
	  : > /tmp/lib.done
	  t "libcopy done ($(ls /tmp/lib | wc -l) files)" ) &
fi
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
	  dispw_x > /dev/null 2>&1
	  dispw -s vdpo 1080p50 > /dev/null 2>&1
	  : > /tmp/dispw.done
	  t "dispw done (early)" ) &
fi
# /dev の一覧を控える (段階 3 以降、最後に呼ばれる S41netparam の時点)。
# udev の再生をやめたときに足りないリンクを、純正どおりの段階 5 と比べる。
if [ "$name" = S41netparam ] && [ "$STAGE" -ge 3 ] 2>/dev/null; then
	ls -laR /dev > "/mnt/extsd/dev-stage$STAGE.txt" 2>/dev/null
fi
mode=$(mode_for "$name")
case "$mode" in
run)
	t "$name start"
	"$1" start > /dev/null 2>&1
	t "$name end"
	exit 0
	;;
udevd)
	# 純正の S10udev から trigger/settle (起動済みの機器への再生) を除いたもの。
	# udevd は残すので、動作中の SD の抜き差し (10-local.rules の udev_handler)
	# は今までどおり。
	t "$name udevd only"
	printf '\000\000\000\000' > /proc/sys/kernel/hotplug
	if [ "$UDEVD_BG" = 1 ]; then
		( /sbin/udevd -d > /dev/null 2>&1; t "udevd up (background)" ) &
	else
		/sbin/udevd -d > /dev/null 2>&1
	fi
	# 再生をやめると udev の規則が作るリンクができない。分かっているのは
	# /dev/rtc (50-udev-default.rules: rtc0 -> rtc)。無いと fork の時計ページが
	# 落ちる (段階 1/2)。ほかは /dev の一覧の差分で確かめる。
	if [ -e /dev/rtc0 ] && [ ! -e /dev/rtc ]; then
		ln -s rtc0 /dev/rtc && t "/dev/rtc link made"
	fi
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

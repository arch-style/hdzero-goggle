#!/bin/sh
# Goggle 2 の起動時間の実験: 純正の /mnt/app/script/rc.sh (9.5.1) の代わりに
# 走る版。カードの直下に置くと、フラッシュの rc.sh の先頭に足した入口
# (2026-10-16) がこれを実行する。中身は純正の rc.sh の写しで、下の設定で
# 選んだ所だけ変える。消すかカードを抜けば純正の rc.sh に戻る。
#
# 必ず最後に exit 0 で終わること。0 以外で終わると入口が純正の rc.sh を
# 続けて走らせ、録画プロセスやアプリが二重に起動する。
#
# 各段階の経過秒をカードの rc-exp.log に書く (読むのは Mac で)。
# カードの整理 (2026-10-29): 動かすのに要るものは fork-system/、消してよいもの
# (ログ、古い版) は fork-deletable/。カード直下に残すのは純正のもの
# (DCIM, FIRMWARE, FSCK, log, HDZG_*.bin などの決まった名前) と、フラッシュの
# 入口が名前で呼ぶ rc-exp.sh, init-exp.sh, early-exp.sh, develop.sh だけ。
SYS=/mnt/extsd/fork-system
DEL=/mnt/extsd/fork-deletable

# --- 実験の設定 (1 = その変更を入れる) ---
# 純正はドライバを入れる前に理由なく 1 秒待つ
SKIP_SLEEP1=1
# 純正は画面を 1080p50 に設定する (実験 1 で 1.38 秒)。外せない: 実験 2
# (2026-10-16) で外すと、fork は lvgl_init の中 (lv_obj_set_style_bg_color に
# NULL、画面のオブジェクトが作れていない) で SIGSEGV、代わりに起動した純正も
# BMI270 の初期化の後で止まり、起動ロゴの後は真っ暗だった。
SKIP_DISPW=0
# 画面の設定を裏で走らせ、その間にドライバと録画プロセスの起動 (共有
# ライブラリの読み込み) を進める。アプリを起動する前には設定の終わりを待つ。
# 実験 3 で確かめる
PARALLEL_DISPW=1
# 内蔵の SD 状態ログをカードの log/ へ写す処理 (約 1.2 秒)。SD カードの
# 認識の記録に使うので消さず、アプリを起動した後に裏で走らせる
LATE_SDSTAT=1
# 記録 (SD 状態ログ、dmesg、アプリのログの写し) を書くまでの待ち。サービスを
# 起動してからの秒数。映像の後の port 1 (HDMI 出力、モーションセンサー) は
# 9.7 秒ごろ、メニューの組み立ては 11 秒ごろに終わるので、7.8 秒 + 4 秒で
# 両方が入る。2026-10-16 夜までは 10 で、ビープが電源から 18 秒だった。
# 2026-10-29 から 0: 映像は 4 秒台、メニューは 5 秒台、port 1 は 8 秒前後に終わり、
# アプリのログ全体は fork-deletable/HDZGOGGLE.log に残る (次の起動で boot-logs/ へ)。
# 写しは「ビープの時点まで」の分。ビープを早めるのが目的 (ユーザーの要望)。
RECORD_DELAY=0
# I2C に触る前に、電源投入からこの秒数になるまで待つ (0 = 待たない)。
# 実験 i2/i3 (2026-10-16): ドライバの読み込みを 4.3 秒や 6.2 秒に始めると、
# I2C port 1 が時間切れで使えなくなり起動画面で止まった。7.7 秒 (i1) は
# 問題なし。i4 では 7.6 秒まで待ち、早すぎることが原因かを確かめる
I2C_NOT_BEFORE=7.6
# I2C を待つ間 (6.1-7.6 秒は何もしていない) に、アプリと録画プロセスが使う
# 共有ライブラリと fork 本体 (合わせて約 11MB) を読んでページキャッシュに
# 載せておく。一覧はカードの preload.txt (preload.goggle2.txt の写し)。
# ファイルを読むだけで部品には触らない。実験 i5 (アプリ最初のログ 10.88 ->
# 10.18 秒)。実験 i6 からは init-exp.sh が S10udev の時点で読むので 0
PRELOAD=0
# 実験 C1: アプリを I2C の関門より前に起動する。アプリは環境変数
# HDZ_I2C_NOT_BEFORE を受け取り、自分の I2C の通信をその時刻まで待たせる
# (src/driver/i2c.c)。関門より前に、ライブラリの読み込み、設定の読み込みなど
# I2C を使わない分を済ませておける。ドライバ (mcp3021, nct75) と録画プロセスは
# これまでどおり関門の後。関門を持たない純正のアプリやそれより前の fork では
# 使わないこと
APP_BEFORE_I2C=1
# アプリの I2C のうち、関門 (I2C_NOT_BEFORE) で待たせる port。固まるのが
# 分かっているのは port 1 (R_I2C) だけで、2 (FPGA、受信チップ) と 3 (HDMI 入力)
# は未確認。C1 は全部待たせる。C2 で "1" だけにして、受信チップの初期化
# (port 2、約 1.1 秒) を関門より前に済ませられるかを見る。
# C1 (全部待たせる) の HDZero の起動: 関門 7.60 秒で受信チップの初期化が始まり
# 8.95 秒まで 1.33 秒、映像は電源から 10.8 秒。C2 (2026-10-16): port 1 だけ
APP_I2C_GATED_PORTS="1"
# 受信チップのバス (port 2) の関門 (uptime 秒。0 = 待たない)。計画 1-2 以降で
# アプリの起動が早まっても、受信チップに触る時刻を今までと同じに保つための安全柵。
# port 2 を 3.5 秒ごろから使って問題なかったのは boot 44、153、154、155 で確認済み
# (アプリ main が 3.46〜3.64 秒、初期化はその 0.08 秒後)。それより早い時刻は
# 未検証なので、計画 1-4 で 1 段ずつ下げて確かめる (efuse の指紋が一致し、
# "no response" が無いこと)。2026-10-20。
# 計画 1-4 の 1 段目 (2026-10-28): 3.3 秒。boot 240〜252 (段階 264〜276) の 13 回で
# 指紋が一致し "no response" 無し。速い側の 5 回は port 2 を 3.30〜3.39 秒に開き
# (3.5 のときは 14 回とも 3.50)、映像 5.68〜5.85 s (3.5 のときは 5.86〜5.92 s)。
# 記録は logs/goggle2/2026-10-28-port2-3.3/。
# 2 段目 (2026-10-28): 3.1 秒。G と A2 の後、速い側の回は 3.3 で 19 ms と 146 ms
# 待っていた (boot 259、260)。boot 262〜272 (段階 286〜296) の 11 回で指紋が一致し
# "no response" 無し、11 回とも送信機の信号を受けた。速い側の 5 回は関門の時点が
# 3.17〜3.24 秒で待ちは 0、映像 5.55〜5.64 s。受信チップを 3.18 秒より前に使った回は
# まだ無い。記録は logs/goggle2/2026-10-28-port2-3.1/。
PORT2_NOT_BEFORE=3.1

# 実験の段階 (init-exp.sh が stage.txt から決めて /tmp/exp-stage に書く)。
# 1: udevd だけ + アプリの I2C は全 port を関門まで待たせる。2: udevd だけ +
# port 1 だけ関門。それ以外: 上の既定。
STAGE=$(cat /tmp/exp-stage 2>/dev/null)
[ -n "$STAGE" ] || STAGE=0
case "$STAGE" in
1) APP_I2C_GATED_PORTS="1 2 3" ;;
2|3|4|5|6|7|8|9|1[0-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) APP_I2C_GATED_PORTS="1" ;;
esac
# 段階 9 以降: init-exp.sh がカードの g2lib/ を /tmp/lib に写すので、アプリと
# 録画プロセスにはそこから読ませる (LD_LIBRARY_PATH の先頭)。
LIBCOPY=0
case "$STAGE" in 9|1[0-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) LIBCOPY=1 ;; esac
# 段階 10/11、13 以降: 案 E。fork 本体が HDZ_DEFER_PORT1 を見て、HDMI 出力
# (IT66121) と IMU の port 1 の初期化を映像の後の別スレッドに回す。
DEFER_PORT1=0
case "$STAGE" in 10|11|1[3-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) DEFER_PORT1=1 ;; esac
# 段階 14 以降: OSD の組み立て (lvgl だけ) をチューナー待ちの前に。
OSD_BEFORE_TUNER=0
case "$STAGE" in 1[4-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) OSD_BEFORE_TUNER=1 ;; esac
# 段階 15 以降: チューナーの efuse (工場で焼かれた較正値) をカードに控え、
# 2 回目からは macro 2 以降の読み出し (約 0.4 秒) を省く。
EFUSE_CACHE=0
case "$STAGE" in 1[5-9]|2[0-9]|[3-9][0-9]|[1-9][0-9][0-9]) EFUSE_CACHE=1 ;; esac
# 段階 21 以降: develop.sh → develop.g2.sh の連鎖 (機種判定、控え、報告、
# 本体の cp で 0.4〜0.5 秒) を通らず、ここから直接 fork を起動する。設定の
# 書き戻しだけは前に同期で行い、控え、wifi-hello、起動しなかったときの純正への
# 切り替え (8 秒後) と動作確認 (30 秒後) は起動の後ろに回す。
FAST_LAUNCH=0
case "$STAGE" in 2[1-9]|[3-9][0-9]|[1-9][0-9][0-9]) FAST_LAUNCH=1 ;; esac
# この起動の通し番号 (カードの boot-logs/count)。アプリのログの写しの名前に使う。
mkdir -p $SYS $DEL/boot-logs 2>/dev/null
BOOTN=$(( $(tr -dc 0-9 < $DEL/boot-logs/count 2>/dev/null) + 1 ))
echo "$BOOTN" > $DEL/boot-logs/count 2>/dev/null

LOG=$DEL/rc-exp.log
t() {
	echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$LOG" 2>/dev/null
}
echo "--- boot $BOOTN from $0 stage=$STAGE FAST=$FAST_LAUNCH LIBCOPY=$LIBCOPY DEFER_PORT1=$DEFER_PORT1 OSD_FIRST=$OSD_BEFORE_TUNER EFUSE_CACHE=$EFUSE_CACHE SKIP_SLEEP1=$SKIP_SLEEP1 SKIP_DISPW=$SKIP_DISPW PARALLEL_DISPW=$PARALLEL_DISPW LATE_SDSTAT=$LATE_SDSTAT I2C_NOT_BEFORE=$I2C_NOT_BEFORE PRELOAD=$PRELOAD APP_BEFORE_I2C=$APP_BEFORE_I2C GATED=$APP_I2C_GATED_PORTS PORT2=$PORT2_NOT_BEFORE" >> "$LOG" 2>/dev/null
t "rc-exp start"

function insmod_all()
{
	insmod /mnt/app/ko/gpio_keys_hdzero.ko
	insmod /mnt/app/ko/rotary_encoder.ko
}

function setup_uart()
{
	echo 0x0300B098 0x00775577 > /sys/class/sunxi_dump/write
	echo 0x0300B0D8 0x22777777 > /sys/class/sunxi_dump/write
	echo 0x0300B0fc 0x35517751 > /sys/class/sunxi_dump/write
}

# dispw -x の代わり。カードの ko/disp.ko (純正 9.5.1 の disp.ko の写しで、
# disp_vdpo_disable() の最後の 1000 ms 待ちを 100 ms に書き換えたもの、
# goggle2-ko/) があればそれを insmod する。この待ちは画面のモードを切り替える
# たび (起動時の 1080p50、入力の切り替えごと) に入っていた。vdpo.ko は純正の
# まま。カードの写しが読めなければ純正の disp.ko に戻る。
function dispw_x()
{
	if [ -f $SYS/ko/disp.ko ]; then
		aww 0x07000160 0x083f10b7
		if insmod $SYS/ko/disp.ko; then
			t "disp.ko from card"
		else
			insmod /mnt/app/ko/disp.ko
			t "disp.ko from card FAILED, stock loaded"
		fi
		mount -t debugfs none /sys/kernel/debug
		insmod /mnt/app/ko/vdpo.ko
	else
		dispw_x
	fi
}

function config_uart_vdpo()
{
	setup_uart
	insmod_all

	if [ "$SKIP_DISPW" = 1 ]; then
		:
	elif [ -e /tmp/dispw.started ]; then
		# init-exp.sh がもう始めている (EARLY_DISPW)。終わりはアプリの前に待つ。
		t "dispw already started by init-exp"
	elif [ "$PARALLEL_DISPW" = 1 ]; then
		( dispw_x; dispw -s vdpo 1080p50; t "dispw done (background)" ) &
		DISPW_PID=$!
	else
		dispw_x
		dispw -s vdpo 1080p50
	fi
}

function write_flashes()
{
	/mnt/app/script/write_flashes.sh
}


function beep()
{
        echo "131">/sys/class/gpio/export
        echo "1">/sys/class/gpio/gpio131/value
        sleep 1
        echo "0">/sys/class/gpio/gpio131/value
}

# ごく短いビープ (2ms) を n 回。実験の終わり (記録が全部カードに書けた時点)
# に 1 回だけ鳴らす。鳴ったら電源を切ってよい。起動時には鳴らさない
# (2026-10-16 の要望)。ブザーは純正の beep と同じ gpio131。
function beep_short()
{
	n=${1:-1}
	[ -e /sys/class/gpio/gpio131 ] || echo "131" > /sys/class/gpio/export 2>/dev/null
	# rc.sh の時点ではまだ出力に設定されていない (アプリが後で設定する)。
	# 設定しないと value を書いても鳴らない (2026-10-16 の起動時のビープ)。
	echo out > /sys/class/gpio/gpio131/direction 2>/dev/null
	# ブザーは GPIO の on/off だけで音量は変えられないので、長さで小さくする
	# (アプリの短いビープは 50ms。100ms と 20ms は大きすぎた)。
	while [ "$n" -gt 0 ]; do
		echo "1" > /sys/class/gpio/gpio131/value
		usleep 2000 2>/dev/null
		echo "0" > /sys/class/gpio/gpio131/value
		n=$((n - 1))
		[ "$n" -gt 0 ] && { usleep 120000 2>/dev/null || sleep 1; }
	done
}

function update_boot()
{
	local boot_file=/mnt/extsd/HDZG_BOOT.bin
	local os_file=/mnt/extsd/HDZG_OS.bin

	if [ -e $boot_file ]; then
		export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/mnt/app
		/mnt/app/ota-burnuboot $boot_file
		rm $boot_file
		fw_setenv hdzero_update "sprite_update"
		fw_setenv bootcmd "run hdzero_update setargs_nor boot_normal"
		beep
	elif [ -e $os_file ]; then
		rm $os_file
		beep
	fi
}

echo "0 4 1 7">/proc/sys/kernel/printk

if [ "$PRELOAD" = 1 ] && [ -f $SYS/preload.txt ]; then
	( while read -r f; do cat "$f" > /dev/null 2>&1; done < $SYS/preload.txt
	  t "preload done" ) &
fi

#config UART and VDPO
config_uart_vdpo
t "config_uart_vdpo done"

#check boot&os file, update if exist
update_boot

#auto write HDZERO_TX.bin / HDZERO_RX.bin /HDZERO_VA.bin when bootup if they are exist on SD card
write_flashes
t "write_flashes done"

# 画面の設定がアプリより前に終わっていること (実験 2: 終わる前だと lvgl_init で
# 落ちる)。init-exp.sh が始めたものは /tmp/dispw.done、ここで始めたものは
# DISPW_PID で待つ。
wait_dispw() {
	if [ -e /tmp/dispw.started ]; then
		i=0
		while [ ! -e /tmp/dispw.done ] && [ $i -lt 100 ]; do
			usleep 50000
			i=$((i + 1))
		done
		t "dispw (early) waited"
	elif [ -n "$DISPW_PID" ]; then
		wait "$DISPW_PID"
		DISPW_PID=""
		t "dispw waited"
	fi
}

app_pid() {
	for d in /proc/[0-9]*; do
		if grep -qs HDZGOGGLE "$d/cmdline" 2>/dev/null; then
			echo "${d#/proc/}"
			return 0
		fi
	done
	return 1
}

# FAST_LAUNCH: develop.g2.sh (develop.sh.vopatch) と同じことを、起動に要る順で
# 最小限に。本体は init-exp.sh が /tmp に写したもの (大きさが合うとき)。
# 0 以外で終われば、呼び出し側が従来どおり develop.sh を使う。
fast_launch() {
	[ -f $SYS/HDZGOGGLE ] || return 1
	REPORT=$DEL/vo-patch.log
	[ -e $SYS/goggle2-restore-setting.sh ] && /bin/sh $SYS/goggle2-restore-setting.sh g2
	SZ=$(ls -l $SYS/HDZGOGGLE | awk '{print $5}')
	if [ -s /tmp/HDZGOGGLE.staged.done ] && [ "$(cat /tmp/HDZGOGGLE.staged.done)" = "$SZ" ] \
	   && [ "$(ls -l /tmp/HDZGOGGLE.staged | awk '{print $5}')" = "$SZ" ] \
	   && mv /tmp/HDZGOGGLE.staged /tmp/HDZGOGGLE 2>/dev/null; then
		APP=/tmp/HDZGOGGLE
	elif cp $SYS/HDZGOGGLE /tmp/HDZGOGGLE 2>/dev/null && chmod +x /tmp/HDZGOGGLE; then
		APP=/tmp/HDZGOGGLE
	else
		return 1
	fi
	"$APP" &
	t "app launched (fast, $APP)"
	( [ -e $SYS/goggle2-backup.sh ] && SYNC_BG=1 /bin/sh $SYS/goggle2-backup.sh g2
	  echo "platform=[fast launch] model=g2" >> $DEL/model-history.txt
	  [ -e $SYS/wifi-hello.sh ] && /bin/sh $SYS/wifi-hello.sh ) > /dev/null 2>&1 &
	# 起動から LAUNCH_CHECK 秒でアプリがいなければ純正で起動し、結果を書いたら
	# /tmp/launch-report.done を置く。記録を書く側 (LATE_SDSTAT) はこれを
	# 待ってからビープを鳴らすので、ビープの後にカードへ書くものは残らない。
	# 2026-10-16 夜までは、さらに 22 秒待ってから pid を書いていた。
	# 2026-10-29 に 8 -> 3 秒: アプリが起動で落ちたのはどれも 1〜2 秒以内で、
	# 3 秒なら確認は services started (7.9 秒ごろ) より前に終わり、ビープを待たせない。
	LAUNCH_CHECK=3
	FAST_USED=1
	( echo "--- boot ---"
	  echo "app: $APP (fast launch from rc-exp.sh, stage $STAGE)"
	  sleep "$LAUNCH_CHECK"
	  if ! app_pid > /dev/null; then
		echo "fork が起動しなかったので純正で起動する"
		/mnt/app/app/HDZGOGGLE &
		sleep 1
	  fi
	  PID=$(app_pid)
	  echo "app pid: ${PID:-not found}"
	  [ -n "$PID" ] && echo "mapped: $(grep -o '[^ ]*libmpp_vo[^ ]*' "/proc/$PID/maps" 2>/dev/null | head -1)"
	  sync; : > /tmp/launch-report.done ) >> "$REPORT" 2>&1 &
	return 0
}

start_app() {
	if [ "$FAST_LAUNCH" = 1 ] && fast_launch; then
		return 0
	fi
	# develop.sh は fork-system/ を先に見る (2026-10-29、入口の 4 本をフォルダへ)。
	DEV=$SYS/develop.sh
	[ -e "$DEV" ] || DEV=/mnt/extsd/develop.sh
	if [ -e "$DEV" ]; then
		"$DEV"
	else
		if [ -e /mnt/extsd/HDZGOGGLE.log ]; then
			/mnt/app/app/HDZGOGGLE > /mnt/extsd/HDZGOGGLE.log 2>&1 &
		else
			#GLOG_minloglevel=3 /mnt/app/app/HDZGOGGLE &  > /dev/null 2>&1
			/mnt/app/app/HDZGOGGLE &
		fi
	fi
}

# 録画プロセスの環境 (LD_LIBRARY_PATH に /lib/libffmpeg と /lib/eyesee-mpp を
# 足す)。純正の順番ではアプリもこれを引き継いでいて、無いと ffmpeg の
# ライブラリが見つからずにアプリが起動しない。アプリを先に起動するときも
# その前に読み、二重に足さないよう一度だけにする。
record_env() {
	[ -n "$RECORD_ENV_DONE" ] && return 0
	source /mnt/app//app/record/record-env.sh
	RECORD_ENV_DONE=1
}

# 関門を持たないアプリ (純正、それより前の fork) を早く起動すると I2C が
# 固まる。カードの fork 本体が関門を持つことは、Mac の check-sd.sh が本体を
# 調べて HDZGOGGLE.features に i2c_gate と書いたときだけ信じる。無ければ
# これまでどおり関門の後に起動する。
if [ "$APP_BEFORE_I2C" = 1 ] && ! grep -qs '^i2c_gate$' $SYS/HDZGOGGLE.features; then
	t "APP_BEFORE_I2C ignored: HDZGOGGLE.features does not say i2c_gate"
	APP_BEFORE_I2C=0
fi

if [ "$APP_BEFORE_I2C" = 1 ]; then
	wait_dispw
	record_env
	if [ "$LIBCOPY" = 1 ]; then
		i=0
		while [ ! -e /tmp/lib.done ] && [ $i -lt 60 ]; do usleep 50000; i=$((i + 1)); done
		if [ -e /tmp/lib.done ]; then
			export LD_LIBRARY_PATH="/tmp/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
			t "libs from /tmp/lib (waited $((i * 50))ms)"
		else
			t "libcopy not done, libs from NOR"
		fi
	fi
	[ "$DEFER_PORT1" = 1 ] && export HDZ_DEFER_PORT1=1
	[ "$OSD_BEFORE_TUNER" = 1 ] && export HDZ_OSD_BEFORE_TUNER=1
	[ "$EFUSE_CACHE" = 1 ] && export HDZ_EFUSE_CACHE=1
	for p in $APP_I2C_GATED_PORTS; do
		export HDZ_I2C${p}_NOT_BEFORE=$I2C_NOT_BEFORE
	done
	if [ "$PORT2_NOT_BEFORE" != 0 ]; then
		export HDZ_I2C2_NOT_BEFORE=$PORT2_NOT_BEFORE
	fi
	# アプリは起動時に /mnt/UDISK/setting.ini を見る (settings.c)。rc_mount が
	# 裏に回る版の rcS (計画 1-5) に備え、UDISK がマウント済みなのを確かめてから
	# 起動する。今の rcS では常にマウント済みで、待ちは 0。
	i=0
	while ! grep -qs ' /mnt/UDISK ' /proc/mounts && [ $i -lt 40 ]; do usleep 50000; i=$((i + 1)); done
	if grep -qs ' /mnt/UDISK ' /proc/mounts; then
		t "UDISK mounted (waited $((i * 50))ms)"
	else
		t "UDISK not mounted after 2 s, launching anyway"
	fi
	[ -f $SYS/app-sampler.sh ] && /bin/sh $SYS/app-sampler.sh &
	start_app
	t "app launched (before I2C; ports $APP_I2C_GATED_PORTS not before $I2C_NOT_BEFORE; port 2 not before $PORT2_NOT_BEFORE)"
fi

# 早すぎる I2C を避ける (I2C_NOT_BEFORE を参照)。
if [ "$I2C_NOT_BEFORE" != 0 ]; then
	while awk -v lim="$I2C_NOT_BEFORE" '{exit !($1 < lim)}' /proc/uptime; do
		usleep 100000
	done
	t "waited until $I2C_NOT_BEFORE for I2C"
fi

#load driver 
if [ "$SKIP_SLEEP1" != 1 ]; then
	sleep 1
fi
insmod /mnt/app/ko/mcp3021.ko
usleep 2000
insmod /mnt/app/ko/nct75.ko
usleep 2000

#set Microphone Bias Control Register
aww 0x050967c0 0x110e6100
t "drivers loaded"

#record process
record_env
if [ -e /mnt/extsd/RECORD.log ]; then
	/mnt/app/app/record/record > /mnt/extsd/RECORD.log 2>&1 &
else
	/mnt/app/app/record/record &
fi
t "record started"
#/mnt/app/app/record/gogglecmd -rec startao
if [ "$LATE_SDSTAT" != 1 ]; then
	/mnt/app/script/sdstat_log_backup.sh
fi
#/mnt/app/app/record/sdstat &

#system led
/mnt/app/script/system_daemon.sh &

#external services
if [ ! -z $(ls -1 /mnt/extsd/hdzgoggle-services-* 2> /dev/null) ]; then
	if [ -e /mnt/extsd/SERVICES.log ]; then
		/mnt/app/services/install.sh > /mnt/extsd/SERVICES.log 2>&1 &
	else
		/mnt/app/services/install.sh &
	fi
elif [ -e /mnt/app/services/startup.sh ]; then
	if [ -e /mnt/extsd/SERVICES.log ]; then
		/mnt/app/services/startup.sh > /mnt/extsd/SERVICES.log 2>&1 &
	else
		/mnt/app/services/startup.sh &
	fi
fi
t "services started"

if [ "$APP_BEFORE_I2C" != 1 ]; then
	wait_dispw

	# アプリが main() に入るまでを外から見る (app-sampler.sh があれば)。
	[ -f $SYS/app-sampler.sh ] && /bin/sh $SYS/app-sampler.sh &

	#start applicaion
	start_app
	t "app launched"
fi

# 後回しにした SD 状態ログの写しを、アプリが立ち上がってから裏で走らせる。
if [ "$LATE_SDSTAT" = 1 ]; then
	# ついでに dmesg をカードに控える (リングバッファは約 16KB で、16 秒ごろの
	# IT66121 の NACK 連打に押し出される前に取る)。画面切り替えの [VDPO] の
	# disable から enable までの間隔 (disp.ko の差し替えの確認) を見る。
	( sleep "$RECORD_DELAY"; /mnt/app/script/sdstat_log_backup.sh > /dev/null 2>&1; t "late sdstat done"; dmesg > $DEL/dmesg-late.txt 2>/dev/null
	  # アプリのログは次の起動で上書きされるので、ここまでの分を写しておく
	  # (映像が出るまでの時刻はこの中)。
	  cp $DEL/HDZGOGGLE.log "$DEL/boot-logs/boot$BOOTN-stage$STAGE.log" 2>/dev/null
	  cp $DEL/dmesg-late.txt "$DEL/boot-logs/boot$BOOTN-stage$STAGE.dmesg" 2>/dev/null
	  # 起動確認の報告 (fast_launch) が書き終わるまで待つ (最大 10 秒)
	  if [ "$FAST_USED" = 1 ]; then
		i=0
		while [ ! -e /tmp/launch-report.done ] && [ $i -lt 100 ]; do usleep 100000; i=$((i + 1)); done
	  fi
	  # カードを使うほど重くなる処理の計測 (fill-probe.sh があれば、2026-10-18)。
	  # fsck.fat -n を含むので数秒かかる。ビープはその後。
	  [ -f $SYS/fill-probe.sh ] && /bin/sh $SYS/fill-probe.sh "$STAGE" "$BOOTN" > /dev/null 2>&1
	  # 上書き領域の rcS の写し (v3) を入れ直す (goggle2-install-rcs.sh があれば、2026-10-20 夜)。
	  # 一度だけ書き、効くのは次の起動から。結果は goggle2-install-rcs/result.txt。
	  [ -f $SYS/goggle2-install-rcs.sh ] && /bin/sh $SYS/goggle2-install-rcs.sh g2 > /dev/null 2>&1
	  # フラッシュの rc.sh と上書き領域の rc.final の入口を入れ直す (goggle2-install-hooks.sh が
	  # あれば、2026-10-29)。一度だけ書き、効くのは次の起動から。結果は goggle2-install-hooks/result.txt。
	  # それまでは develop.sh からしか呼ばれず、rc-exp.sh で起動する限り届かなかった。
	  [ -f $SYS/goggle2-install-hooks.sh ] && /bin/sh $SYS/goggle2-install-hooks.sh g2 > /dev/null 2>&1
	  # カーネルとブートローダの情報を読む (kernel-probe.sh があれば、2026-10-19)。読むだけ。
	  [ -f $SYS/kernel-probe.sh ] && /bin/sh $SYS/kernel-probe.sh "$STAGE" "$BOOTN" > /dev/null 2>&1
	  # initcall_debug を一度だけ入れて測り、戻す (env-initcall/ があれば、2026-10-19)。
	  [ -f $SYS/env-initcall.sh ] && /bin/sh $SYS/env-initcall.sh > /dev/null 2>&1
	  # カードのカーネルを試す起動手順と、その段の進行 (env-bootcmd/ があれば、2026-10-19)。
	  [ -f $SYS/env-bootcmd.sh ] && /bin/sh $SYS/env-bootcmd.sh > /dev/null 2>&1
	  C=/sys/devices/system/cpu/cpu0/cpufreq
	  { echo "== cpufreq at records $(cut -d' ' -f1 /proc/uptime)"
	    echo "gov=$(cat $C/scaling_governor) cur=$(cat $C/scaling_cur_freq) min=$(cat $C/scaling_min_freq) trans=$(cat $C/stats/total_trans)"
	    cat $C/stats/time_in_state; } >> "$DEL/boot-logs/early-stage$STAGE.txt" 2>/dev/null
	  # init-exp.sh が起動中だけ最高に固定した最低クロックを、元に戻す。
	  cat $C/cpuinfo_min_freq > $C/scaling_min_freq 2>/dev/null
	  # カードから起動する作ったカーネル (hdzlike7 以降) は起動時の governor が performance。
	  # 記録を書いた後は純正と同じ ondemand に戻す (純正カーネルでは ondemand のまま何もしない)。
	  [ "$(cat $C/scaling_governor 2>/dev/null)" = performance ] && echo ondemand > $C/scaling_governor 2>/dev/null
	  # 書き終えた合図の 2 ms のビープ。鳴ったら電源を切ってよい。止めたいときは
	  # fork-system/no-beep を置く (2026-10-29、一度止めて戻した)。
	  sync; t "records written (boot $BOOTN)"; sync
	  [ -e $SYS/no-beep ] || beep_short 1 ) &
fi

exit 0

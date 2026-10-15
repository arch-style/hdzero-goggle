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

LOG=/mnt/extsd/rc-exp.log
t() {
	echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$LOG" 2>/dev/null
}
echo "--- boot SKIP_SLEEP1=$SKIP_SLEEP1 SKIP_DISPW=$SKIP_DISPW PARALLEL_DISPW=$PARALLEL_DISPW LATE_SDSTAT=$LATE_SDSTAT I2C_NOT_BEFORE=$I2C_NOT_BEFORE PRELOAD=$PRELOAD" >> "$LOG" 2>/dev/null
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

function config_uart_vdpo()
{
	setup_uart
	insmod_all

	if [ "$SKIP_DISPW" = 1 ]; then
		:
	elif [ "$PARALLEL_DISPW" = 1 ]; then
		( dispw -x; dispw -s vdpo 1080p50; t "dispw done (background)" ) &
		DISPW_PID=$!
	else
		dispw -x
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

if [ "$PRELOAD" = 1 ] && [ -f /mnt/extsd/preload.txt ]; then
	( while read -r f; do cat "$f" > /dev/null 2>&1; done < /mnt/extsd/preload.txt
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
source /mnt/app//app/record/record-env.sh
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

# 画面の設定を裏で走らせたなら、アプリより前に終わっていること。
if [ -n "$DISPW_PID" ]; then
	wait "$DISPW_PID"
	t "dispw waited"
fi

#start applicaion
if [ -e /mnt/extsd/develop.sh ]; then
	/mnt/extsd/develop.sh
else
	if [ -e /mnt/extsd/HDZGOGGLE.log ]; then
		/mnt/app/app/HDZGOGGLE > /mnt/extsd/HDZGOGGLE.log 2>&1 &
	else
		#GLOG_minloglevel=3 /mnt/app/app/HDZGOGGLE &  > /dev/null 2>&1
		/mnt/app/app/HDZGOGGLE &
	fi
fi
t "app launched"

# 後回しにした SD 状態ログの写しを、アプリが立ち上がってから裏で走らせる。
if [ "$LATE_SDSTAT" = 1 ]; then
	( sleep 10; /mnt/app/script/sdstat_log_backup.sh > /dev/null 2>&1; t "late sdstat done"; sync ) &
fi

exit 0

#!/bin/sh
# Goggle 2 の起動時間の実験: カードから /tmp への写しを、S10udev (2.6 秒ごろ) より
# 1 秒早く始める (計画 1-2、2026-10-20)。
#
# 呼ぶのは上書き領域の rcS の写し (v2)。rcS の先頭でカードを /mnt/extsd に
# マウントした直後 (1.6 秒ごろ) に、これを裏で呼ぶ。rcS が v1 (ca520542) のまま
# なら誰も呼ばず、init-exp.sh が今までどおり S10udev で写す。
#
# ここで始めたものは /tmp に印を置く (libcopy.started、appcopy.started、
# dispw.started)。init-exp.sh は印があれば自分では始めない。終わりの印
# (/tmp/lib.done、/tmp/HDZGOGGLE.staged.done、/tmp/dispw.done) は init-exp.sh が
# 作るものと同じで、rc-exp.sh がそれを待つ。
#
# 消せば今までどおり。カードに書くのは init-exp.log への行だけ。

# 画面の設定 (disp.ko の insmod と 1080p50) もここで始めるか (計画 1-3)。
# 1.6 秒ごろに画面を触るのは未検証なので、既定は 0 (init-exp.sh が S10udev で行う)。
# 2026-10-28 (計画 A2): 1 にして試す。disp.ko は depends が空、vdpo.ko は disp だけ
# (modinfo)。dispw -s は debugfs に書くだけで /mnt/app も S00mpp のモジュールも使わない。
EARLY_DISPW_AT_RCS=1

LOG=/mnt/extsd/init-exp.log
t() {
	echo "$(cut -d' ' -f1 /proc/uptime) early: $*" >> "$LOG" 2>/dev/null
}
t "start"

# 共有ライブラリ 47 本 (g2lib/) を /tmp/lib へ。init-exp.sh の EARLY_LIBCOPY と同じ。
if [ -d /mnt/extsd/g2lib ] && [ ! -e /tmp/libcopy.started ]; then
	: > /tmp/libcopy.started
	( mkdir -p /tmp/lib
	  cp /mnt/extsd/g2lib/*.so* /tmp/lib/ 2>/dev/null
	  # FAT にはシンボリックリンクが置けないので、soname の名前はここで作る。
	  while read -r link target; do
		[ -n "$link" ] && [ -f "/tmp/lib/$target" ] && ln -sf "$target" "/tmp/lib/$link"
	  done < /mnt/extsd/g2lib/links.txt
	  : > /tmp/lib.done
	  t "libcopy done ($(ls /tmp/lib | wc -l) files)" ) &
fi

# fork 本体を /tmp/HDZGOGGLE.staged へ。init-exp.sh の EARLY_APPCOPY と同じ。
if [ -f /mnt/extsd/HDZGOGGLE ] && [ ! -e /tmp/appcopy.started ]; then
	: > /tmp/appcopy.started
	( rm -f /tmp/HDZGOGGLE.staged.done
	  if cp /mnt/extsd/HDZGOGGLE /tmp/HDZGOGGLE.staged && chmod +x /tmp/HDZGOGGLE.staged; then
		ls -l /mnt/extsd/HDZGOGGLE | awk '{print $5}' > /tmp/HDZGOGGLE.staged.done
		t "appcopy done"
	  fi ) &
fi

# 画面の設定 (計画 1-3)。init-exp.sh の EARLY_DISPW と同じ手順。
if [ "$EARLY_DISPW_AT_RCS" = 1 ] && [ ! -e /tmp/dispw.started ]; then
	: > /tmp/dispw.started
	t "dispw start (at rcS)"
	( echo 0x0300B098 0x00775577 > /sys/class/sunxi_dump/write
	  echo 0x0300B0D8 0x22777777 > /sys/class/sunxi_dump/write
	  echo 0x0300B0fc 0x35517751 > /sys/class/sunxi_dump/write
	  if [ -f /mnt/extsd/ko/disp.ko ]; then
		aww 0x07000160 0x083f10b7
		if insmod /mnt/extsd/ko/disp.ko; then
			t "disp.ko from card"
		else
			insmod /mnt/app/ko/disp.ko
			t "disp.ko from card FAILED, stock loaded"
		fi
		mount -t debugfs none /sys/kernel/debug
		# /mnt/app は rc_preboot がマウントする (boot 158 で 2.33 秒) ので、
		# ここ (boot 158 で 2.08 秒、161 で 1.89 秒) ではまだ無いことがある。カードの ko/ に純正の
		# vdpo.ko (2026-10-15 のバックアップの app/ko/vdpo.ko、md5 6eebb1df) を
		# 置き、そちらを先に使う (計画 A2、2026-10-28)。
		if [ -f /mnt/extsd/ko/vdpo.ko ] && insmod /mnt/extsd/ko/vdpo.ko; then
			t "vdpo.ko from card"
		else
			insmod /mnt/app/ko/vdpo.ko
			t "vdpo.ko from /mnt/app"
		fi
	  else
		dispw -x
	  fi
	  dispw -s vdpo 1080p50 > /dev/null 2>&1
	  : > /tmp/dispw.done
	  t "dispw done (at rcS)" ) &
fi

wait
t "end"
exit 0

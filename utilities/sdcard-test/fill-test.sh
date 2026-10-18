#!/bin/sh
#
# SD カードを使うほど起動が重くなるかを測るための、カードの準備 (Mac 側で実行)。
# ゴーグル側は fill-probe.sh (カードの直下) が、起動ごとに boot-logs/fill-stage<段階>.txt
# に結果を書く。集計は fill-report.py。
#
#   fill-test.sh files  /Volumes/<カード>        filltest/ にダミーのフォルダを作る
#   fill-test.sh space  /Volumes/<カード> <GB>   filltest/space-*.bin (3 GB ずつ) で使用量を増やす
#   fill-test.sh clean  /Volumes/<カード>        filltest/ を消す (本物のファイルは触らない)
#   fill-test.sh status /Volumes/<カード>        今の本数と使用量
#
# files が作るもの (本物のフォルダは触らない。ゴーグルの再生画面や録画の番号にも
# 出ない場所):
#   filltest/log-0100 ... log-3000   2026-09-08.log.<番号> (sdstat_log_backup.sh の形)
#   filltest/dcim-0100 ... dcim-3000 hdz_<4 桁>.ts (録画と同じ形)
# 中身は 1 バイトなので、1 本が 1 クラスタ (64 KB) を使う。全部で 8800 本、約 550 MB。

CMD=${1:?usage: fill-test.sh files|space|clean|status /Volumes/<カード> [GB]}
SD=${2:?usage: fill-test.sh $CMD /Volumes/<カード>}
SIZES="0100 0300 1000 3000"

[ -d "$SD" ] || { echo "$SD がない"; exit 1; }
case "$SD" in /Volumes/*) ;; *) echo "$SD は /Volumes/ の下ではない"; exit 1 ;; esac
T="$SD/filltest"

status() {
	echo "== $SD"
	df -h "$SD" | tail -1
	for d in log DCIM/100HDZRO boot-logs; do
		[ -d "$SD/$d" ] && printf '  %-16s %6d 本\n' "$d" "$(ls -A "$SD/$d" | wc -l)"
	done
	printf '  %-16s %6d 本\n' "(直下)" "$(ls -A "$SD" | wc -l)"
	if [ -d "$T" ]; then
		for d in "$T"/*; do
			if [ -d "$d" ]; then
				printf '  %-16s %6d 本\n' "filltest/${d##*/}" "$(ls -A "$d" | wc -l)"
			elif [ -f "$d" ]; then
				printf '  %-16s %s\n' "filltest/${d##*/}" "$(du -h "$d" | cut -f1)"
			fi
		done
	else
		echo "  filltest/ なし"
	fi
}

case "$CMD" in
files)
	mkdir -p "$T" || exit 1
	for n in $SIZES; do
		cnt=$(expr "$n" + 0)
		for kind in log dcim; do
			d="$T/$kind-$n"
			if [ -d "$d" ] && [ "$(ls -A "$d" | wc -l)" -eq "$cnt" ]; then
				echo "$kind-$n はある"
				continue
			fi
			rm -rf "$d"; mkdir -p "$d" || exit 1
			printf '%s を作る (%d 本) ... ' "$kind-$n" "$cnt"
			i=0
			while [ $i -lt $cnt ]; do
				if [ $kind = log ]; then
					printf x > "$d/2026-09-08.log.$i"
				else
					printf x > "$d/$(printf 'hdz_%04d.ts' $i)"
				fi
				i=$((i + 1))
			done
			echo ok
		done
	done
	;;
space)
	GB=${3:?usage: fill-test.sh space /Volumes/<カード> <GB>}
	mkdir -p "$T" || exit 1
	avail_kb=$(df -k "$SD" | tail -1 | awk '{print $4}')
	if [ $((GB * 1024 * 1024 + 1024 * 1024)) -gt "$avail_kb" ]; then
		echo "空きが足りない (空き $((avail_kb / 1024 / 1024)) GB、1 GB は残す)"; exit 1
	fi
	# FAT32 は 1 本 4 GB 未満なので、3 GB ずつ (端数は最後の 1 本) に分ける。
	echo "filltest/space-*.bin に ${GB} GB を書く (カードの速さ次第で数分)"
	rm -f "$T"/space*.bin
	left=$GB; i=1
	while [ "$left" -gt 0 ]; do
		n=3; [ "$left" -lt 3 ] && n=$left
		f=$(printf '%s/space-%02d.bin' "$T" $i)
		printf '  %s (%d GB) ... ' "${f##*/}" "$n"
		mkfile "${n}g" "$f" || { echo "失敗"; exit 1; }
		echo ok
		left=$((left - n)); i=$((i + 1))
	done
	;;
clean)
	[ -d "$T" ] || { echo "filltest/ はない"; exit 0; }
	rm -rf "$T" && echo "filltest/ を消した"
	;;
status) ;;
*) echo "files|space|clean|status のどれか"; exit 1 ;;
esac

# macOS が足す ._ を消してから一覧を出す
find "$SD" -name '._*' -type f -delete 2>/dev/null
status

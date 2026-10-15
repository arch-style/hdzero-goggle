#!/bin/sh
# fork のアプリが main() に入るまで (2026-10-16 の測定で起動から約 1.7 秒、
# うちライブラリの結び付けは約 0.3 秒) に何をしているかを、/proc を外から
# 50ms ごとに読んで記録する。読むだけで、アプリには何もしない。
# rc-exp.sh が develop.sh より前に裏で起動する。結果はカードの app-sample.log。
#
# 列: 起動からの秒 状態(R=走っている S=眠っている D=I/O 待ち) CPU 時間(tick)
#     読み込み済みの .so の数 スレッド数 待っているカーネル関数 (wchan)

OUT=/mnt/extsd/app-sample.log
echo "--- boot" >> "$OUT"

# アプリのプロセスが現れるまで待つ (最大 10 秒)。
P=""
i=0
while [ -z "$P" ] && [ $i -lt 200 ]; do
	for p in /proc/[0-9]*; do
		if [ "$(readlink $p/exe 2>/dev/null)" = /tmp/HDZGOGGLE ]; then
			P=$p
			break
		fi
	done
	[ -n "$P" ] || usleep 50000
	i=$((i + 1))
done
[ -n "$P" ] || { echo "app not found" >> "$OUT"; exit 0; }

s=$(cat $P/stat); r=${s##*) }; set -- $r
echo "pid ${P#/proc/} start_tick ${20}" >> "$OUT"

# 3 秒間、またはプロセスが消えるまで。
n=0
while [ $n -lt 60 ] && [ -d "$P" ]; do
	up=$(cut -d' ' -f1 /proc/uptime)
	s=$(cat $P/stat 2>/dev/null) || break
	r=${s##*) }
	set -- $r
	st=$1
	cpu=$(( ${12} + ${13} ))
	so=$(grep -c '\.so' $P/maps 2>/dev/null)
	th=$(ls $P/task 2>/dev/null | wc -l)
	wc=$(cat $P/wchan 2>/dev/null)
	echo "$up $st cpu=$cpu so=$so th=$th wchan=$wc" >> "$OUT"
	usleep 50000
	n=$((n + 1))
done
sync

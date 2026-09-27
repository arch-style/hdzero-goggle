#!/bin/sh
# crash.log (ゴーグルが落ちたときに SD ルートに書かれる) を、ビルドの
# デバッグ情報で関数名と行番号に直す。
#   crash-decode.sh /Volumes/NO\ NAME/crash.log [build_goggle2/out/HDZGOGGLE]
LOG=$1
BIN=${2:-build_goggle2/out/HDZGOGGLE}
HERE=$(cd "$(dirname "$0")/../.." && pwd)
# 最後のクラッシュだけ
awk '/^=== crash/{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$LOG" > /tmp/crash.last
head -2 /tmp/crash.last
# 実行ファイルの text 範囲 (非 PIE なのでアドレスはそのまま)
RANGE=$(grep ' r-xp ' /tmp/crash.last | grep 'HDZGOGGLE$' | head -1 | cut -d' ' -f1)
LO=$((0x${RANGE%-*})); HI=$((0x${RANGE#*-}))
PC=$(grep -o 'pc=0x[0-9a-f]*' /tmp/crash.last | cut -d= -f2)
LR=$(grep -o 'lr=0x[0-9a-f]*' /tmp/crash.last | cut -d= -f2)
ADDRS="$PC $LR"
for a in $(sed -n '/^stack:/,/^maps:/p' /tmp/crash.last | grep -o '0x[0-9a-f]*'); do
	v=$((a)); [ $v -ge $LO ] && [ $v -lt $HI ] && ADDRS="$ADDRS $a"
done
echo "--- pc, lr, then return addresses found on the stack (app code only)"
docker run --rm -v "$HERE":/src -w /src hdzero-dev /src/toolchain/bin/arm-buildroot-linux-musleabihf-addr2line -f -i -p -C -e "$BIN" $ADDRS 2>/dev/null
echo "--- pc, lr and stack return addresses by library (+offset)"
# ベンダのカーネルは maps に余分な列を足すので、パスは最後の列
grep ' r-xp ' /tmp/crash.last | awk '{print $1, $NF}' > /tmp/crash.maps
for a in $PC $LR $(sed -n '/^stack:/,/^maps:/p' /tmp/crash.last | grep -o '0x[0-9a-f]*'); do
	v=$((a))
	while read r p; do
		lo=$((0x${r%-*})); hi=$((0x${r#*-}))
		[ $v -ge $lo ] && [ $v -lt $hi ] && printf '%s %s+0x%x\n' "$a" "$(basename "$p")" $((v - lo))
	done < /tmp/crash.maps
done

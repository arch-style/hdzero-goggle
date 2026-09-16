#!/bin/sh
#
# SD カードをゴーグルに挿す前の点検 (Mac 側で実行)。
#
#   utilities/sdcard-test/check-sd.sh /Volumes/<SDカード>
#
# 見るもの:
#   1. 起動時に純正 rc.sh が自動でフラッシュに書いてしまうファイルが無いこと
#      (これが唯一の「壊す」経路。develop.sh より前に、確認なしで走る)
#   2. develop.sh が sh から実行できる形であること (LF 改行、#!/bin/sh)
#   3. HDZGOGGLE が ARM の ELF で、手元のビルド成果物と同一であること
# 何か引っかかれば非 0 で終わる。

SD=${1:?usage: check-sd.sh /Volumes/<SDカード>}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
# 9.5.0 ベースの移植 (../hdzero-goggle-9.5, 持ち主の 9.5.0 に合わせたもの) を
# 優先し、無ければ 9.6 ベースのビルドと比べる。
BUILT=""
for cand in "$ROOT/../hdzero-goggle-9.5/out/HDZGOGGLE" "$ROOT/build_goggle/out/HDZGOGGLE"; do
	if [ -f "$cand" ]; then BUILT="$cand"; break; fi
done
fail=0

say()  { printf '  %s\n' "$*"; }
ng()   { printf 'NG  %s\n' "$*"; fail=1; }
ok()   { printf 'ok  %s\n' "$*"; }

if [ ! -d "$SD" ]; then
	ng "$SD がディレクトリではない"
	exit 1
fi

echo "== $SD の中身 =="
ls -la "$SD" | sed 's/^/  /'
echo

echo "== 1. 起動時にフラッシュへ自動書き込みするファイル =="
# rc.sh -> write_flashes.sh: RX/VA を mtd8/9/10 に書く (ビープの後、確認なし)
# rc.sh -> update_boot(): ブートローダ / OS を書く
# update_goggle.sh: HDZERO_GOGGLE-*.bin (メニュー操作で走るが、置かない)
# services/install.sh: hdzgoggle-services-* を /mnt/app に展開する
for f in HDZGOGGLE_VA.bin HDZGOGGLE_RX.bin HDZG_BOOT.bin HDZG_OS.bin \
         HDZBOXPRO_VA.bin HDZBOXPRO_RX.bin HDZB_BOOT.bin HDZB_OS.bin \
         HDZERO_TX.bin; do
	if [ -e "$SD/$f" ]; then ng "$f がある: 起動しただけでフラッシュに書かれる"; fi
done
for pat in "HDZERO_GOGGLE-*.bin" "HDZERO_GOGGLE2-*.bin" "HDZERO_BOXPRO*.bin" "hdzgoggle-services-*"; do
	for f in "$SD"/$pat; do
		[ -e "$f" ] && ng "$(basename "$f") がある: ファームウェア/サービスの更新入力になる"
	done
done
# 挙動を変えるトリガ (壊さないが、テストの条件が変わる)
for f in self_test.txt no_dial.txt pclk_phase.cfg vclk_phase_inv.txt test.ini; do
	if [ -e "$SD/$f" ]; then ng "$f がある: アプリの挙動を変えるトリガ。外す"; fi
done
[ $fail -eq 0 ] && ok "自動書き込みのトリガは無い"
echo

echo "== 2. develop.sh =="
D="$SD/develop.sh"
if [ ! -f "$D" ]; then
	ng "develop.sh が無い (無ければ純正アプリが起動するだけ)"
else
	if LC_ALL=C grep -q "$(printf '\r')" "$D"; then
		ng "develop.sh に CR (\\r) が含まれる。#!/bin/sh が壊れて実行されない。LF で保存し直す"
	else
		ok "改行は LF"
	fi
	if [ "$(head -c 9 "$D")" = "#!/bin/sh" ]; then
		ok "1 行目が #!/bin/sh"
	else
		ng "1 行目が #!/bin/sh ではない: $(head -1 "$D")"
	fi
	if grep -q "HDZGOGGLE" "$D"; then
		if grep -q '/mnt/extsd/HDZGOGGLE ' "$D" || grep -q '/mnt/extsd/HDZGOGGLE$' "$D"; then
			say "種別: fork を SD から起動する (stage1)"
		elif grep -q "setting.ini.orig\" /mnt/app/setting.ini" "$D"; then
			say "種別: setting.ini を書き戻す (restore)"
		else
			say "種別: 純正アプリを起動する (stage0)"
		fi
	fi
	if grep -q 'mtd_debug\|fw_setenv\|ota-burn\|/dev/mtd\|write_flashes\|update_goggle\|update_vtx\|gogglecmd' "$D"; then
		ng "develop.sh にフラッシュ書き込み系のコマンドが含まれる"
	else
		ok "フラッシュ書き込み系のコマンドは無い"
	fi
	# rc.sh は develop.sh を同期実行するので、& を付けないと rc.sh が戻らない。
	# 二つの grep を || でつなぐ。BRE の \| で括ると、途中に来た $ が行末の
	# 指定ではなくただの文字になり、当たらなくなる。
	if grep -q '"\$1" &$' "$D" || grep -q 'HDZGOGGLE.*&$' "$D"; then
		ok "アプリはバックグラウンド起動"
	else
		ng "アプリ起動行が & で終わっていない"
	fi

	# ログを書けないときに起動そのものを諦めないこと。リダイレクトが失敗した
	# コマンドはシェルが実行しないので、read-only のカードで起動画面のまま
	# 止まる原因になる。
	if grep -q 'WRITABLE' "$D"; then
		ok "カードが read-only でも起動する (書けないときはログを捨てる)"
	else
		ng "カードが書けないと起動しない作りになっている"
	fi
fi
echo

echo "== 3. HDZGOGGLE (fork バイナリ) =="
A="$SD/HDZGOGGLE"
if [ ! -f "$A" ]; then
	say "無い。develop.sh が stage1 なら純正アプリにフォールバックする"
else
	if file "$A" | grep -q 'ELF 32-bit LSB.*ARM'; then
		ok "ARM 32bit ELF"
	else
		ng "ARM の ELF ではない: $(file "$A")"
	fi
	if [ -n "$BUILT" ]; then
		if [ "$(md5 -q "$A")" = "$(md5 -q "$BUILT")" ]; then
			ok "$BUILT と同一"
		else
			ng "$BUILT と違う (古いビルド? コピー忘れ?)"
		fi
	else
		say "ビルド成果物が無いので同一性は確認できない"
	fi
	if strings "$A" | grep -q 'HDZGOGGLE2'; then
		say "(参考) バイナリ内に HDZGOGGLE2 の文字列あり。文字列だけでは判断できないので、ビルド時の -DHDZ_GOGGLE=ON を確認すること"
	fi
fi
echo

if [ $fail -eq 0 ]; then
	echo "OK: このまま挿してよい"
else
	echo "NG: 上の NG を直してから挿す"
fi
exit $fail

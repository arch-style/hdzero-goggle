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
#   4. 両機用 (develop.sh.dual) なら、v1 用の HDZGOGGLE.v1 が build_goggle の
#      ビルドと同一で Goggle 2 用ではないこと、持ち主の setting.ini の控えが
#      原本と同じであること
# 何か引っかかれば非 0 で終わる。

SD=${1:?usage: check-sd.sh /Volumes/<SDカード>}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
# 9.5.0 ベースの移植 (../hdzero-goggle-9.5, 持ち主の 9.5.0 に合わせたもの) を
# 優先し、無ければ 9.6 ベースのビルドと比べる。
BUILT=""
for cand in "$ROOT/../hdzero-goggle-9.5/out/HDZGOGGLE" "$ROOT/build_goggle/out/HDZGOGGLE"; do
	if [ -f "$cand" ]; then BUILT="$cand"; break; fi
done
# 両機用の develop.sh では HDZGOGGLE は Goggle 2 用、v1 用は HDZGOGGLE.v1。
BUILT_G2="$ROOT/../hdzero-goggle/build_goggle2/out/HDZGOGGLE"
BUILT_V1="$ROOT/../hdzero-goggle/build_goggle/out/HDZGOGGLE"
# 借りた日に取った持ち主の setting.ini (Mac に 7 つある控えはすべてこれ)。
ORIG_SHA=5eb27b713aa1284193888fefdb57928456b7cdb5
DUAL=0
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
	if grep -q 'develop.g2.sh' "$D"; then
		DUAL=1
	fi
	if grep -q "HDZGOGGLE" "$D"; then
		if [ $DUAL -eq 1 ]; then
			say "種別: 両機用 (Goggle 2 は develop.g2.sh、v1 は HDZGOGGLE.v1)"
		elif grep -q 'ab-logs' "$D"; then
			say "種別: 純正と fork を交互に起動する測定用 (abtest)"
		elif grep -q '/mnt/extsd/HDZGOGGLE ' "$D" || grep -q '/mnt/extsd/HDZGOGGLE$' "$D"; then
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

# rc-exp.sh: Goggle 2 のフラッシュの rc.sh に足した入口 (2026-10-16) が、
# あれば純正の rc.sh の代わりに実行する。0 以外で終わると純正の rc.sh が
# 続けて走り、録画やアプリが二重に起動する。
X="$SD/rc-exp.sh"
if [ -f "$X" ]; then
	echo "== rc-exp.sh (Goggle 2 の rc.sh の代わり) =="
	if LC_ALL=C grep -q "$(printf '\r')" "$X"; then
		ng "rc-exp.sh に CR (\\r) が含まれる"
	fi
	if [ "$(grep -v '^[[:space:]]*$' "$X" | tail -1)" = "exit 0" ]; then
		ok "最後が exit 0 (純正の rc.sh は続けて走らない)"
	else
		ng "rc-exp.sh の最後が exit 0 ではない"
	fi
	# APP_BEFORE_I2C は、アプリ自身が I2C を関門まで待たせるときだけ安全。
	# rc-exp.sh は HDZGOGGLE.features の i2c_gate を見て判断するので、ここで
	# 本体と突き合わせる。
	FEAT="$SD/HDZGOGGLE.features"
	if grep -qs '^i2c_gate$' "$FEAT"; then
		if strings "$SD/HDZGOGGLE" 2>/dev/null | grep -q 'HDZ_I2C_NOT_BEFORE'; then
			ok "HDZGOGGLE は I2C の関門を持つ (HDZGOGGLE.features と一致)"
		else
			ng "HDZGOGGLE.features は i2c_gate と言うが、HDZGOGGLE に関門が無い。早く起動すると I2C が固まる"
		fi
	elif grep -q '^APP_BEFORE_I2C=1' "$X"; then
		say "APP_BEFORE_I2C=1 だが HDZGOGGLE.features が無いので、アプリは関門の後に起動する"
	fi
	say "設定: $(grep -E '^(SKIP_[A-Z0-9]+|LATE_[A-Z]+|PARALLEL_[A-Z]+|I2C_[A-Z_]+|PRELOAD|APP_[A-Z0-9_]+)=' "$X" | tr '\n' ' ')"
	# goggle2-install-hooks.sh は、フラッシュの rc.sh と上書き領域の rc.final に
	# 入口を書く唯一のカード側スクリプト。写しが手元の goggle2-flash/ と同一で、
	# まだ DONE が無いときだけ、次の起動で一度書く。
	IH="$SD/goggle2-install-hooks.sh"
	if [ -f "$IH" ]; then
		if [ -e "$SD/goggle2-install-hooks/DONE" ]; then
			say "goggle2-install-hooks.sh は済み (DONE あり)"
		elif [ "$(md5 -q "$SD/goggle2-flash/rc.sh" 2>/dev/null)" = "$(md5 -q "$ROOT/utilities/sdcard-test/goggle2-flash/rc.sh")" ] && [ "$(md5 -q "$SD/goggle2-flash/rc.final" 2>/dev/null)" = "$(md5 -q "$ROOT/utilities/sdcard-test/goggle2-flash/rc.final")" ]; then
			ok "goggle2-install-hooks.sh: 次の Goggle 2 の起動で rc.sh と rc.final の入口をフラッシュに書く (写しは手元と同一)"
		else
			ng "goggle2-install-hooks.sh があるが、カードの goggle2-flash/rc.sh か rc.final が手元と違う"
		fi
	fi
	# カードの ko/disp.ko は、rc-exp.sh と init-exp.sh の dispw_x が純正の
	# disp.ko の代わりに insmod する (goggle2-ko/README.md)。手元の写しと
	# 同一でなければ入れない (壊れた .ko を insmod すると画面が出ない)。
	KO="$SD/ko/disp.ko"
	if [ -f "$KO" ]; then
		if [ "$(md5 -q "$KO")" = "$(cut -d' ' -f1 < "$ROOT/utilities/sdcard-test/goggle2-ko/disp.ko.md5" 2>/dev/null)" ]; then
			ok "ko/disp.ko は goggle2-ko/disp.ko と同一 (1 秒待ちを 100 ms にした写し)"
		else
			ng "ko/disp.ko が goggle2-ko/disp.ko と違う。消すか入れ直す"
		fi
	else
		say "ko/disp.ko は無い (純正の disp.ko、画面の切り替えごとに 1 秒待つ)"
	fi
	# g2lib/: アプリと録画プロセスの共有ライブラリの写し (段階 9 以降、init-exp.sh
	# が /tmp/lib に写す)。手元の一覧 (goggle2-ko/g2lib.md5) と同一か。
	if [ -d "$SD/g2lib" ]; then
		bad=0
		while read -r sum name; do
			[ "$(md5 -q "$SD/g2lib/$name" 2>/dev/null)" = "$sum" ] || bad=$((bad + 1))
		done < "$ROOT/utilities/sdcard-test/goggle2-ko/g2lib.md5"
		if [ $bad -eq 0 ]; then
			ok "g2lib/ の $(wc -l < "$ROOT/utilities/sdcard-test/goggle2-ko/g2lib.md5" | tr -d ' ') ファイルは手元の写しと同一"
		else
			ng "g2lib/ に手元と違うか無いファイルが $bad ある"
		fi
	fi
	echo
fi

if [ $DUAL -eq 1 ]; then
	G="$SD/develop.g2.sh"
	if [ ! -f "$G" ]; then
		say "develop.g2.sh が無い。Goggle 2 では純正が起動する"
	else
		if LC_ALL=C grep -q "$(printf '\r')" "$G"; then
			ng "develop.g2.sh に CR (\\r) が含まれる"
		fi
		if grep -q 'mtd_debug\|fw_setenv\|ota-burn\|/dev/mtd\|write_flashes\|update_goggle\|update_vtx\|gogglecmd' "$G"; then
			ng "develop.g2.sh にフラッシュ書き込み系のコマンドが含まれる"
		else
			ok "develop.g2.sh (Goggle 2 側) にフラッシュ書き込み系のコマンドは無い"
		fi
	fi
	echo

	# Goggle 2 の控えを取るスクリプト。読むだけであること、v1 では動かないことを見る。
	K="$SD/goggle2-backup.sh"
	if [ -f "$K" ]; then
		# コメント行と、捨て先の /dev/null は見ない。
		body=$(grep -v '^[[:space:]]*#' "$K" | sed 's#/dev/null##g')
		if LC_ALL=C grep -q "$(printf '\r')" "$K"; then
			ng "goggle2-backup.sh に CR (\\r) が含まれる"
		fi
		if printf '%s\n' "$body" | grep -q 'mtd_debug\|fw_setenv\|flash_erase\|flashcp\|nandwrite\|of=/dev\|insmod\|write_flashes\|update_goggle\|update_vtx'; then
			ng "goggle2-backup.sh にフラッシュ書き込み系のコマンドが含まれる"
		elif printf '%s\n' "$body" | grep -q '>[[:space:]]*"*/dev/\|>[[:space:]]*"*/mnt/app\|tar -x\|tar x'; then
			ng "goggle2-backup.sh がデバイスか /mnt/app に書いている"
		elif printf '%s\n' "$body" | grep -Eq '^[[:space:]]*(cp|mv|rm)[[:space:]].*/mnt/app[^[:space:]]*"?[[:space:]]*$'; then
			ng "goggle2-backup.sh が /mnt/app のファイルを置き換えるか消している"
		else
			ok "goggle2-backup.sh (Goggle 2 の控え) は読むだけ"
		fi
		if ! printf '%s\n' "$body" | grep -q 'HDZGOGGLE2'; then
			ng "goggle2-backup.sh に機種の確認が無い。v1 でも動いてしまう"
		fi
		if grep -q 'goggle2-backup.sh' "$D"; then
			if [ -f "$SD/goggle2-backup/DONE" ]; then
				say "控えはもう取ってある (goggle2-backup/DONE)。次の起動は setting.ini を写すだけ"
			else
				say "次に Goggle 2 で起動すると控えを取る。初回だけ映像まで数十秒余計にかかる"
			fi
		else
			say "goggle2-backup.sh はあるが develop.sh から呼ばれない (develop.sh.dual を入れ直す)"
		fi
		echo
	fi

	# 設定の書き戻し。書くのは /mnt/app/setting.ini だけであることを見る。
	W="$SD/goggle2-restore-setting.sh"
	if [ -f "$W" ]; then
		body=$(grep -v '^[[:space:]]*#' "$W" | sed 's#/dev/null##g')
		if printf '%s\n' "$body" | grep -q 'mtd_debug\|fw_setenv\|flash_erase\|flashcp\|nandwrite\|of=/dev\|insmod\|write_flashes\|update_goggle\|update_vtx\|>[[:space:]]*"*/dev/\|tar -x\|tar x'; then
			ng "goggle2-restore-setting.sh にフラッシュ書き込み系のコマンドが含まれる"
		elif [ "$(printf '%s\n' "$body" | grep -c '/mnt/app/')" -ne 3 ] || ! printf '%s\n' "$body" | grep -q '^DST=/mnt/app/setting.ini$'; then
			ng "goggle2-restore-setting.sh が setting.ini 以外の /mnt/app に触れているかもしれない"
		elif ! printf '%s\n' "$body" | grep -q 'HDZGOGGLE2'; then
			ng "goggle2-restore-setting.sh に機種の確認が無い"
		else
			ok "goggle2-restore-setting.sh は /mnt/app/setting.ini だけを書く"
		fi
		RS="$SD/goggle2-restore"
		if [ -e "$RS/DONE" ]; then
			say "書き戻しは済んでいる: $(tail -1 "$RS/result.txt" 2>/dev/null)"
		elif [ ! -f "$RS/setting.ini" ]; then
			say "goggle2-restore/setting.ini が無い。書き戻しは起きない"
		elif [ "$(md5 -q "$RS/setting.ini")" = "$(cut -d' ' -f1 < "$RS/setting.ini.md5" 2>/dev/null)" ]; then
			say "次に Goggle 2 で起動すると setting.ini を書き戻す ($(wc -l < "$RS/setting.ini" | tr -d ' ') 行)"
		else
			ng "goggle2-restore/setting.ini と setting.ini.md5 が合わない (ゴーグル側でも書かずに止まる)"
		fi
		if ! grep -q 'goggle2-restore-setting.sh' "$D"; then
			say "goggle2-restore-setting.sh はあるが develop.sh から呼ばれない"
		fi
		echo
	fi

	echo "== 3. HDZGOGGLE.v1 (v1 で起動する fork) =="
	A="$SD/HDZGOGGLE.v1"
	if [ ! -f "$A" ]; then
		say "無い。v1 では純正アプリが起動する"
	else
		if file "$A" | grep -q 'ELF 32-bit LSB.*ARM'; then
			ok "ARM 32bit ELF"
		else
			ng "ARM の ELF ではない: $(file "$A")"
		fi
		# どちら用かはビルドに残るドライバのソース名で見る。言語ファイルを
		# 埋め込んでからは v1 用にも "HDZERO_GOGGLE2" (訳文) が入るので、
		# goggle2 の文字列の有無では見分けられない。
		if strings "$A" | grep -q 'driver/hardware-goggle2\.c'; then
			ng "hardware-goggle2.c が入っている: Goggle 2 用のバイナリ。v1 では動かさない"
		elif strings "$A" | grep -q 'driver/hardware-goggle\.c\|driver/hardware\.c'; then
			ok "v1 用 (Goggle 2 のドライバは入っていない)"
		else
			ng "どちら用か分からない (ドライバのソース名が見つからない)"
		fi
		if [ -f "$BUILT_V1" ] && [ "$(md5 -q "$A")" = "$(md5 -q "$BUILT_V1")" ]; then
			ok "$BUILT_V1 と同一"
		else
			ng "$BUILT_V1 と違う (古いビルド? コピー忘れ?)"
		fi
	fi
	echo

	echo "== 4. 持ち主の setting.ini の控え =="
	O="$SD/goggle-backup/setting.ini.orig"
	if [ ! -f "$O" ]; then
		ng "goggle-backup/setting.ini.orig が無い。このまま v1 で起動すると fork が変えた後の設定を原本として控えてしまう。Mac の logs/goggle-v1/*/goggle-backup/setting.ini.orig を先に入れる"
	elif [ "$(shasum "$O" | cut -d' ' -f1)" = "$ORIG_SHA" ]; then
		ok "借りた日の原本と同じ (${ORIG_SHA%${ORIG_SHA#??????????}})"
	else
		ng "借りた日の原本 (${ORIG_SHA%${ORIG_SHA#??????????}}) と違う"
	fi
	echo

	echo "== 5. HDZGOGGLE (Goggle 2 で起動する fork) =="
	A="$SD/HDZGOGGLE"
	if [ -f "$A" ]; then
		if strings "$A" | grep -q 'driver/hardware-goggle2\.c'; then
			ok "Goggle 2 用 (v1 では起動されない)"
		else
			ng "Goggle 2 用ではない。両機用の develop.sh では HDZGOGGLE は Goggle 2 で起動される"
		fi
		if [ -f "$BUILT_G2" ] && [ "$(md5 -q "$A")" = "$(md5 -q "$BUILT_G2")" ]; then
			ok "$BUILT_G2 と同一"
		else
			say "$BUILT_G2 と違う (Goggle 2 側。v1 の試験には関係しない)"
		fi
	else
		say "無い。Goggle 2 では develop.g2.sh の決めた通り (純正にフォールバック)"
	fi
	echo
fi

if [ $DUAL -eq 0 ]; then
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
fi

if [ $fail -eq 0 ]; then
	echo "OK: このまま挿してよい"
else
	echo "NG: 上の NG を直してから挿す"
fi
exit $fail

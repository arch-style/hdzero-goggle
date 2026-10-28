# Goggle 2 起動高速化: 後回しにした小さな手 (引き継ぎ、2026-10-28)

## この資料を渡された会話へのお願い

HDZero Goggle 2 の「電源から HDZero の映像まで」を、**新しい区画には手を加えずに**少しずつ縮める作業の続きです。
対象は SD カードのスクリプト、fork のアプリ (カードから起動)、上書き領域に既にある rc.final と rcS の写しだけ。
カーネル、u-boot、env、DT、rootfs は触らない (2026-10-20 に作り直したカーネルで NOR を壊した。下の「失敗」)。

1. この資料、`CLAUDE.md` (自動で読み込まれる)、`docs/goggle2-handoff.md`、`docs/goggle2-speedup-overview.html` を読む。
2. 下の手を、1 回に 1 つだけ変えて試す。各手の前に「何を変える、見込み、外れたときの形、戻し方」を書いて、ユーザーの確認を取る。
3. 判定は「変えたもの自体の時刻」で、同じ種類の回同士で比べる (下の「測り方」)。

返答は日本語。

## いまの状態 (2026-10-20 夜に復帰、10-28 時点で変化なし)

- フラッシュ: 純正 9.5.1 (カーネル #69) + app 区画の rc.sh の入口 (md5 cb87f258) + 上書き領域の rc.final (efe8f153) と rcS v3 (fc9058fe) + 設定 (setting.ini、205892861a)。
- カード:
  - **HDZERO_BOOT** (32 GB): 普段と測定に使う。段階 263、boot 239 まで記録済み (2026-10-28 にカードで確認。初版の「185、161」は誤り)。
  - **HDZCARD** (64 GB、元 NO NAME2): カードからの起動用 (純正カーネルの像)。今回の作業では使わない。
  - **PHOENIX**: PhoenixCard の復旧カード。中身は md5 で確認済み (`~/LocalCodes/hdzg-os-unpacked/card/layout-phoenix-image.txt`)。
- 両カードのスクリプトはリポジトリ `utilities/sdcard-test/` と同じ。Mac の控えは `~/LocalCodes/hdzero-card-staging/HDZERO_BOOT-2026-10-20/`。
- PhoenixCard で戻したときは、HDZERO_BOOT の `goggle2-restore/DONE`、`goggle2-install-hooks/DONE`、`goggle2-install-rcs/DONE` を外して 2 回起動すれば全部戻る。

## 直近の値

| 起動 | 種類 | rcS 開始 | アプリ起動 | 映像 (電源基準) |
|---|---|---|---|---|
| boot 158 | 遅い側 (816 MHz が約 1 s) | 1.86 | 3.28 | 5.94 |
| boot 161 | 速い側 (816 MHz が 0) | 1.53 | 3.09 | 5.94 |

時刻はカーネル起動からの秒。映像だけ電源基準 (内部の時計、+約 1.3〜1.4 s)。ストップウォッチではスイッチから約 7.5 s (差の内訳は未測定)。

## 後回しにした手 (勧める順)

| 手 | 何を | 書く場所 | 見込み | 外れたときの形 | 戻し方 |
|---|---|---|---|---|---|
| **1-4** | 受信チップ (I2C port 2) の関門 `PORT2_NOT_BEFORE` を 3.5 → 3.3 → 3.1 と 1 段ずつ下げる。各段 2 回以上 | カード `rc-exp.sh` | 速い側で 0.1〜0.2 s | 初期化失敗 (10 回の再試行で遅れる、または映像なし)。電源を入れ直す | 値を戻す |
| A1 | init-exp.sh の S10udev での記録 (dmesg の写し、cpufreq、cmdline、`init-exp.sh:154-167` 付近) を、dispw を始めた後に回す | カード `init-exp.sh` | 0.05〜0.1 s | 記録が欠けるだけ | .prev を戻す |
| A2 | 画面の設定 (dispw) を rcS の時点 (2.1 s ごろ) で始める。`early-exp.sh` の `EARLY_DISPW_AT_RCS=1` | カード `early-exp.sh`、`ko/` | 0.05〜0.1 s | 映像が出ない (lvgl_init の前提)。電源を入れ直す | 旗を 0 に |
| F | 受信チップの初期化 0.73 s の中身を読んで縮める | fork `src/driver/dm6302.c` | 0.05〜0.15 s (未確認) | 初期化失敗。電源を入れ直す | 前の本体を戻す |
| G | 初期化完了から映像まで 0.27 s、main からチューナー開始まで 0.12 s の中身を読む | fork `src/core/main.c` ほか | 不明 | — | 前の本体を戻す |

### 1-4 の根拠 (新しく分かったこと)

boot 161 (速い側) で、アプリの関門の行が `i2c: port 2 not before 3.50s after kernel start (now 3.33s)` だった。
つまり速い側の回では、受信チップの初期化が関門 3.5 s を待っている (約 0.17 s)。遅い側 (boot 158、now 3.53) では効いていない。
受信チップを 3.5 s ごろから使って問題なかったのは boot 44、153、154。それより早い時刻は未検証。
判定: efuse の指紋が `ea4dc8cf` / `3c0f97a7` と一致、"no response" が無い、映像が出る。

### A2 の注意 (前の会話で見つけた危険)

`early-exp.sh:65` は `/mnt/app/ko/vdpo.ko` を insmod するが、/mnt/app は rc_preboot がマウントし、boot 158 では 2.33 s に終わる。
early-exp.sh は 2.08 s に始まるので競走になる。vdpo.ko が入らないと dispw が失敗し、fork が lvgl_init で落ちる (実験 2 と同じ)。
対策: 純正の vdpo.ko (`logs/goggle2/2026-10-15-flash-backup/mnt_app.tar` の `app/ko/vdpo.ko`) をカードの `ko/` に置き、そこから入れる。
disp.ko はすでにカードから入れている。`aww` と `dispw` は rootfs の `/usr/sbin/` にあるので rcS の時点で使える (展開した rootfs で確認)。

### F の材料

boot 158 のアプリのログから: Init0 まで 0.12 s (うち `dm6302.c:2144-2146` のリセットの固定待ち 100 ms)、EFUSE1 0.11 s、
Init1〜14 で 0.23 s、DCOC 1+2 で 0.13 s、M0 で 0.12 s。両チップを 1 つずつ処理している所を、EFUSE1_both と同じ形で束ねられるかを読む。
受信チップのバスは 800 kHz が上限 (1.2 MHz は FPGA がついて来ない、`dm6302.c:2075-2081`)。

### G を読んだ結果 (2026-10-28、ゴーグルでは未測定)

- `aww` と `awr` は rootfs の `/usr/sbin/` にあるシェルスクリプト (`echo addr val > /sys/class/sunxi_dump/write` と読み返し)。
  アプリは `system()` で呼ぶので、1 回にシェル 2 つと cat。ログでは 1 回 8〜21 ms。
- boot 158 と 161 のどちらも、受信チップの初期化完了から映像までに aww が 10 回、合計 83 ms (main のスレッドで直列)。
  main の関門からチューナーのスレッド開始までに aww 3 回 (36〜38 ms) と twi2_probe (awr/aww で 39 ms)。
- 直したもの: `src/util/system.c` に `reg_write()` を足し、`reg_read()` とともに sysfs を直接読み書きする形にした。
  呼び出しは `hardware-goggle2.c`、`app_state.c`、`self_test.c`、`gpadc.c` (init と on)、`i2c.c` (twi2_probe)。
  `dm6302.c:2098` (受信チップのバスの速さ、初期化の前に 9〜21 ms、M0 の後に 8 ms) は F の範囲なので触っていない。
- ビルド: HDZGOGGLE sha1 `51d2ad86` (控え `~/LocalCodes/hdzero-card-staging/builds/HDZGOGGLE.51d2ad86.regwrite`)。
  前の本体は `ee9c3985` (同じフォルダの上の `HDZERO_BOOT-2026-10-20/HDZGOGGLE`)。
- 見込み: 初期化完了から映像まで約 80 ms 短く (どちらの種類の回でも)。チューナー開始までの約 75 ms は、関門 (1-4) が効いている間は隠れる。
- 判定: アプリのログで `DM6302 after init` から `picture at` までの間隔と、`reg write` の行が出ていること (`System Execute: aww` が残っていないこと)。

### 準備したカードの旗 (2026-10-28、どちらも既定 0。カードにはまだ入れていない)

- A1: `init-exp.sh` の `RECORDS_AFTER_START=1` で、S10udev の記録を写しと dispw を始めた後に裏で行う。stage の行は S10udev に入った時刻に変えた。
- A2: `early-exp.sh` は、カードの `ko/vdpo.ko` (純正、md5 6eebb1df、控え `~/LocalCodes/hdzero-card-staging/a2-ko/vdpo.ko`) があればそれを入れる。旗は `EARLY_DISPW_AT_RCS=1`。

### 基準の 78 回と 1-4 の開始 (2026-10-28)

- カードは段階 263、boot 239 まで進んでいた (boot 162〜239 は同じ本体 ee9c3985 と同じスクリプト)。記録は `logs/goggle2/2026-10-28-baseline/` (表は `table-boot154-239.txt`)。
- 速い側の回は 78 回中 17 回。映像は 5.86〜5.92 s、関門で 49〜281 ms 待っていた。遅い側の回は 5.91〜6.12 s。
- 初期化完了から映像までは 262〜271 ms で、回の種類によらず揃っている (G の判定に使える)。
- 指紋が出た 71 回はすべて一致し、"no response" は 0。映像の行が無い回は、7.6 s 前に電源を切った回か、HDZero 以外のソースで起動した回だった。
- 1-4 の 1 段目: カードの `rc-exp.sh` (md5 4401bc55) で、段階 264〜279 だけ `PORT2_NOT_BEFORE=3.3`。前の版は `~/LocalCodes/hdzero-card-staging/rc-exp.sh.before-1-4-3.3`。

### 1-4 の 1 段目の結果と G の開始 (2026-10-28)

- 関門 3.3 (boot 240〜252、段階 264〜276、本体 ee9c3985): 13 回とも指紋が一致し、"no response" は 0。
  速い側の 5 回は port 2 を 3.30〜3.39 s に開いた (3.5 のときは 14 回とも 3.50)。映像は 5.68〜5.85 s (3.5 のときは 5.86〜5.92 s)。遅い側は 5.91〜6.13 s で変わらず。
  5 回中 4 回は 3.3 より後に関門に着いており、待ったのは boot 249 (103 ms) だけ。記録は `logs/goggle2/2026-10-28-port2-3.3/`。
  この 13 回は送信機の信号を受けていない (Signal accquired が 0)。電波を受けての映像は、この関門ではまだ見ていない。
- `rc-exp.sh` の既定を 3.3 にした (md5 4f41eaf3)。3.1 は未検証。
- G の試し: 段階 277 から、カードの本体を 51d2ad86 にした。前の本体 ee9c3985 の控えは Mac にだけある。
- カードの空きは 37 MB (DCIM の録画が 29 GB)。

### G の結果と A1 の開始 (2026-10-28)

- G (本体 51d2ad86、関門 3.3、boot 253〜255、3 回とも遅い側):
  - 初期化完了から映像まで 182〜194 ms (前は 78 回で 262〜271 ms)。
  - 関門からチューナーのスレッド開始まで 15〜18 ms (boot 158 では 91 ms)。twi2_probe は 39 ms から 1 ms に。
  - 映像は 5.81〜5.94 s (遅い側の前の値は 5.91〜6.13 s。3 回なので幅はまだ言えない)。
  - 書いた値は aww のときと同じ並びで、失敗の行は無い。残る aww は dm6302.c の 2 回だけ。
  - 指紋は一致し、"no response" は 0。送信機の信号は無し。
  - 記録は `logs/goggle2/2026-10-28-g-regwrite/`。
- A1: 段階 280 から、カードの `init-exp.sh` (md5 4af6c25d) を `RECORDS_AFTER_START=1` にした。前の版は `~/LocalCodes/hdzero-card-staging/init-exp.sh.before-a1`。

### A1 の結果と A2 の開始 (2026-10-28)

- A1 (boot 256〜258。256 は記録の前に電源が切られた):
  - S01app から dispw の開始まで 0.09〜0.12 s (前の 64 回の平均 0.145、A1 の 3 回の平均 0.103)。得は約 0.04 s で、見込み 0.05〜0.1 s より小さい。
  - S01app からアプリの起動まで 0.47〜0.57 s (前の 61 回は 0.48〜0.65 s)。3 回では揺れと区別できない。
  - early-stage の記録 (dmesg、cpufreq、cmdline) は欠けずに書かれていた。旗は 1 のまま残す。
  - boot 257 と 258 は送信機の信号を受けて映像が出た (Signal accquired 5 回、ユーザーも正常と確認)。ただしどちらも遅い側の回で、関門 3.3 が効く速い側の回での受信はまだ見ていない。
  - 記録は `logs/goggle2/2026-10-28-a1-records/`。
- A2: 段階 283 から、カードの `early-exp.sh` (md5 de530417) を `EARLY_DISPW_AT_RCS=1` にし、`ko/vdpo.ko` (md5 6eebb1df) を置いた。前の版は `~/LocalCodes/hdzero-card-staging/early-exp.sh.before-a2`。
  - 前提の確認: disp.ko は depends が空、vdpo.ko は disp だけ。dispw -s は debugfs に書くだけ (展開した rootfs の `/usr/sbin/dispw`)。
  - 見込み: 起動は rc-exp の開始 (A1 で 3.16 s) に縛られるので、得は 0.05 s 前後が上限の見込み。

### A2 の結果 (2026-10-28)

- A2 (boot 259〜261。259 と 260 は速い側、261 は遅い側):
  - dispw の完了は 2.19〜2.61 s (A1 の回は 3.21〜3.27 s)。disp.ko と vdpo.ko はどちらもカードから入った。
  - S01app からアプリの起動まで 0.42〜0.44 s (A1 の回は 0.47〜0.57 s、前の 61 回は 0.48〜0.65 s)。
  - rc-exp の開始から起動までは 0.10 s (A1 の回は 0.15 s)。いまの起動は rc-exp の開始 (rc.final の S スクリプトの連鎖) に縛られている。
  - 映像は 5.66 / 5.68 s (速い側)、5.80 s (遅い側)。
  - 3 回とも信号を受けて映像が出た (ユーザーも正常と確認)。指紋は一致し、"no response" は 0。
  - 速い側の 2 回は、関門 3.3 で 19 ms と 146 ms 待った。速い側で電波を受けたのは、関門 3.3 ではこの 2 回が初めて。
  - 記録は `logs/goggle2/2026-10-28-a2-dispw-at-rcs/`。旗は 1 のまま残す。
- 次の候補: 関門 3.1 (G と A2 で、速い側では関門で待つようになった)。それと、rc-exp の開始を早める (S01app から rc-exp の開始まで約 0.34 s)。

### 1-4 の 2 段目の開始 (2026-10-28)

- 段階 286〜305 だけ `PORT2_NOT_BEFORE=3.1` にした (カードの `rc-exp.sh` md5 10fb402a)。段階 306 からは 3.3 に戻る。前の版は `~/LocalCodes/hdzero-card-staging/rc-exp.sh.before-1-4-3.1`。

### 1-4 の 2 段目の結果と、この日のまとめ (2026-10-28)

- 関門 3.1 (boot 262〜272): 11 回とも指紋が一致し、"no response" は 0。11 回とも送信機の信号を受けた。
  速い側の 5 回は関門の時点が 3.17〜3.24 s で、待ちは 0。映像は 5.55〜5.64 s。遅い側の 6 回は 5.76〜5.89 s。
  受信チップを 3.18 s より前に使った回はまだ無い。記録は `logs/goggle2/2026-10-28-port2-3.1/`。
- カードの `rc-exp.sh` (md5 0dff6976) の既定を 3.1 にした (段階の限定は外した)。
- boot 270 では受信チップの初期化が OSD の組み立てより先に終わり、main がチューナーを待たなかった (`wait for the tuner bus 0ms`)。
  ほかの 10 回は 40〜332 ms 待っていて、まだ受信チップの初期化 (F) が律速。

| 段 | 速い側の映像 | 遅い側の映像 |
|---|---|---|
| 朝の状態 (boot 162〜239、78 回) | 5.86〜5.92 s | 5.91〜6.13 s |
| 関門 3.3 | 5.68〜5.85 s | 変わらず |
| + G (aww を sysfs へ) | — | 5.81〜5.94 s |
| + A1 (記録を裏へ) | — | 5.84〜5.87 s |
| + A2 (dispw を rcS で) | 5.66〜5.68 s | 5.80 s |
| + 関門 3.1 | **5.55〜5.64 s** | **5.76〜5.89 s** |

残り: F (受信チップの初期化、別の会話)。rc-exp の開始を早める (S01app から rc-exp の開始まで約 0.34 s) のは、まだ案だけ。

## 範囲外 (この作業ではやらない)

- 映像用メモリ ion の 512 MB を縮める (195 ms): env の書き換えが要る。
- カーネルの作り直し、DT、ブートローダ: `docs/goggle2-card-boot.md` の「次の手」の規則 (NOR を触らない検証用の仕組みが先)。
- UDISK と mdev -s を裏へ (計画 1-5): 試して効果なし (取り合い)。

## 測り方

- 1 回の起動で 1 つの変更。HDZERO_BOOT を挿して電源、2 ms のビープが鳴ったら電源を切り、カードを Mac へ。
- 読むもの: `rc-exp.log`、`init-exp.log`、`boot-logs/boot<N>-stage<S>.log` (アプリ)、`boot-logs/early-stage<S>.txt` (dmesg と cpufreq)。
- 回の種類: early-stage の `== cpufreq at S10udev` の `816000` の値。約 100 なら遅い側、0 なら速い側。同じ種類同士で比べる。足りなければ「まだ判断できない」。
- 記録はリポジトリの `logs/goggle2/<日付>-<名前>/` に写す (git には入らない)。
- カードを挿す前に `utilities/sdcard-test/check-sd.sh /Volumes/HDZERO_BOOT`。macOS の `._*` は消す。
- ゴーグルの再起動は電源ボタンだけ (SSH の reboot は戻らない)。SSH は自宅の Wi-Fi が要る (設定の Wi-Fi はコワーキングのもの)。

## 失敗の記録 (要点)

- 2026-10-19: u-boot の bootcmd を長く書き換えて起動しなくなった (128 バイトの配列からはみ出した見込み)。
- 2026-10-20: 作り直したカーネルをカードから起動し、NOR を壊した。PhoenixCard で復旧。
- 詳しくはメモリの `feedback-verify-before-asserting.md` の Mistake log。

## 詳しい資料

- `docs/goggle2-handoff.md`: 全体の引き継ぎ (段階 1 の結果、計画)。
- `docs/goggle2-speedup-overview.html`: 仕組みの解説 (区画の図、起動の流れ)。
- `docs/goggle2-card-boot.md`: カード起動の調べ物と、カーネルの次の手。
- `utilities/sdcard-test/README.md` の 5d 節: カードの作り方とスクリプトの役目。
- 記録: `logs/goggle2/2026-10-19-restore/` (boot 154〜159)、`2026-10-20-reinstall/` (boot 160、161)。

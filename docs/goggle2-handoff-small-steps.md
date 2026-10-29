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
  - **2026-10-29 のカードの整理のあと、この手順はそのままでは動かない。** 4 本のスクリプト (`goggle2-install-hooks.sh`、`goggle2-install-rcs.sh`、`goggle2-restore-setting.sh`、`goggle2-backup.sh`) と `goggle2-flash/`、DONE のフォルダは `fork-deletable/` にあり、rc-exp.sh と develop.sh は `fork-system/` を探す。スクリプトの中のパスはカード直下のまま。使う前に、パスを `fork-system/` に直して移す必要がある (未対応)。

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

### F の 1 段目 (F1) の準備 (2026-10-28 夜、ゴーグルでは未測定)

受信チップの初期化の内訳 (boot 262〜272 の 11 回、`thread: tuner init` から `DM6302 after init` まで 0.72〜0.83 s):

| 区間 | 時間 | 中身 (`dm6302.c`) |
|---|---|---|
| 開始→Init0 完了 | ~117 ms | ResetRF の後の固定待ち 100 ms + 1 ms、書き 12 回 |
| EFUSE1 both | 100〜160 ms | 14 語。1 語 = 書き 1 回 + SPI_Read 2 回。SPI_Read 1 回 = バースト 1 + I2C 読み 8 回 |
| Init1〜14 | 195〜330 ms | SPI_Write 562 回 (sel 0 で両チップ同時)。1 回 ≈ 0.4 ms (M0 の 239 回が 124 ms) |
| EFUSE2 1+2 | 12〜35 ms | |
| DCOC 1 + 2 | 130〜150 ms | チップごとに書き 62、読み 8、usleep 18 ms。両チップを 1 つずつ |
| M0 | 122〜128 ms | 書き 239 回 |
| aww ×2 | ~17 ms | バスの速さの切り替え。シェル経由で 8〜9 ms ずつ |

F1 はチップに送るレジスタ列を変えない範囲だけ (本体 `7bc02ae5`、控え `~/LocalCodes/hdzero-card-staging/builds/HDZGOGGLE.7bc02ae5.f1-lean`。前の本体は同じ場所の `HDZGOGGLE.51d2ad86.regwrite`):

1. `dm6302_bus_write()` を aww から `reg_write()` (sysfs 直接) に。見込み −17 ms。
2. efuse の読み (`efuse_read_both()`) を `SPI_Read_byte()` に。ready ビットとデータは各チップの最下位バイトだけなので、FPGA の 0x98 と 0x9c の 2 回だけ読む (前は 0x98〜0x9f の 8 回)。SPI コマンドもレジスタも同じ。見込み −45 ms (28 回 × 1.6 ms)。判定は指紋 `ea4dc8cf` / `3c0f97a7`。
3. DCOC の結果 (rx1/rx2 の i と q、チップごと) を `DCOC n: rx1 i=.. q=..` の行に記録する。F2 (両チップ同時) の比較用。時間は変わらない。
4. 初期化の最後 (バスを 200 kHz に戻した後) に FPGA のブリッジを読むだけの確認 `spi_bridge_probe()`: 0x98〜0x9f の 1 バイトずつとブロック読みが同じか、0x91/0x92 が読み返せるか。`SPI bridge:` の 2 行。F3 (1 回の SPI アクセスの I2C 回数を減らす) ができるかの材料。書き込みは無し。

- 外れたときの形: 2 で指紋が合わない (efuse の読み違い)、または "no response"。4 のブロック読みで FPGA の I2C が詰まると、その後の FPGA 読み書き (RSSI など) が失敗する。どちらも電源を入れ直し、前の本体に戻す。
- 判定: `EFUSE1 both, s1` → `EFUSE1 both done` の間隔 (前は 100〜160 ms)、指紋、"no response" 無し、`System Execute: aww` が無く `reg write 0x05002814` が 2 行、映像、信号。
- 次の候補 (F2 以降、1 回に 1 つ): DCOC を両チップ同時 (sel 0 で共通の書き、結果の書き戻しだけチップごと。見込み −65 ms、判定は 3 の値の比較)。DCOC の usleep(1000) ×8 を元のコメントの 500 us に (−8 ms)。Init0 のリセット後 100 ms を 50 ms に (−50 ms、根拠なし。外れると no response の再試行か、指紋が合わない)。F3 はブリッジの確認の結果次第。

### F1 の結果 (2026-10-28、boot 273〜275、本体 7bc02ae5。273 と 275 は速い側、274 は遅い側)

- 指紋は 3 回とも一致、"no response" は 0、3 回とも信号を受けて映像が出た (ユーザーも正常と確認)。記録は `logs/goggle2/2026-10-28-f1-lean/`。
- `EFUSE1 both, s1` → `EFUSE1 both done`: 49 / 110 / 47 ms (前の 11 回は 100〜160 ms)。
- `thread: tuner init` → `DM6302 after init`: 0.724 / 0.724 / 0.721 s (前の 11 回は 0.708〜0.832 s)。
- `System Execute: aww` は消え、`reg write 0x05002814` が初期化の前後に 1 行ずつ。
- main の `wait for the tuner bus` は 145 / 65 / 211 ms で、まだ受信チップの初期化が律速。
- DCOC の値 (F2 の比較用): rx1 i の下 16 bit と q の下 16 bit は 3 回とも同じ (chip1 rx1 i=..0021 q=..0009、rx2 i=..0005 q=..1100、chip2 rx1 i=..001f q=..0d00/0f00、rx2 i=..0059/005b q=..0047)。上 16 bit は数 LSB 揺れる。
- ブリッジの確認: 0x98〜0x9f の 1 バイトずつとブロック読みは一致したが、どちらも全部 0x00 だったので、連続読みが本当に次の番地へ進んでいるかはまだ言えない (0 でない値のときに見る必要がある)。0x91/0x92 は書いた値 (f0 6f) が読み返せた。
- 映像の時刻は 3 回では前の幅 (速い側 5.55〜5.64、遅い側 5.76〜5.89) と区別できない。

### 映像が「出て、消えて、また出る」件 (2026-10-28、動画 IMG_1017.MOV とログから)

- 動画 (120 fps) の明るさの推移: 1.4 s ロゴ、4.4 s 消灯、5.57 s 映像、6.40〜6.75 s 暗転、6.77 s から映像。動画の 0 がほぼ電源投入 (ロゴの 1.4 s とブートローダの 1.4 s、映像の 5.57 s と速い側の 5.55〜5.64 s が合う)。
- ログ (boot 236〜275 で毎回同じ): `picture at` の約 1.13〜1.15 s 後に `CAM_mode: 1->4` (`hardware-goggle2.c:941`、HDZERO_detect) → `OLED: Display off` → `vdpo: already 720p60` → `pclk_phase_set 0` → 52 ms 後に `OLED: Display on`。
- 意味: アプリは起動時に受信映像の形式を 720p60 (`CAM_MODE` の初期値、`msp_displayport.c:29`) と仮定して表示を作る。送信機の設定パケット (`parser_config` → `camTypeDetect`、0x33 = VR_540P60 = 4) で本当の形式 (540p60) が分かると、`Display_720P60_50_t(VR_540P60)` で表示を作り直す (パネル消灯、FPGA 0x80=1、DM5680_SetFPS、MFPGA_Set540P60、点灯)。この作り直しが 2. の後の暗転。
- 見る側では `OLED: Display on` (55.924) から映像 (動画 5.57 s) まで約 0.4 s、作り直しの消灯から再表示まで約 0.37 s。どちらもリンクの再同期と見えるが、ログに印は無く未確認。
- 検出は peripheral スレッド (`thread.c:274`、menu pages の後に開始、2 ms × 50 周期) が行うので、設定パケットの到着より遅れている可能性がある (到着時刻はログに無い)。
- 案 (未実施): 前回の CAM_MODE と cam_4_3 をカードに覚えておき、次の起動では最初からその形式で表示を作る。合っていれば作り直しが起きず、暗転が消え、正常な映像が約 1.2 s 早く (5.6 s ごろ) 見える。違う送信機なら今まで通り作り直す。1080p の送信機では A2 の早い dispw (720p60) が外れて dispw がもう 1 回走る点に注意。

### カメラ形式の控え (cam-mode.txt) の準備 (2026-10-28 夜、ゴーグルでは未測定)

- 本体 `b337e693` (控え `~/LocalCodes/hdzero-card-staging/builds/HDZGOGGLE.b337e693.cam-mode-cache`、前は `HDZGOGGLE.7bc02ae5.f1-lean`)。F1 の変更も含む。
- 変更: `hardware-goggle2.c` に `cam_mode_cache_load()` / `cam_mode_cache_store()`。カードに `/mnt/extsd/cam-mode.txt` ("<CAM_MODE> <cam_4_3>") があれば、`settings_load()` の直後 (`main.c`) に読んで `CAM_MODE`、`cam_4_3`、HDZERO_detect の「前回の値」をそれに合わせる。受け付けるのは Display_720P60_50_t の経路の形式 (0, 1, 4, 5) だけ。HDZERO_detect が形式か比率の変化を見たら、ファイルを書き直す。ファイルが無ければ何もしない (関門は env ではなくファイルの有無)。
- カードには `cam-mode.txt` = `4 1` (boot 275 のログの CAM_MODE=4、cam_4_3=1) を置いた。段階 300 から。
- 見込み: `switch to bw:... CAM_MODE=4 4:3=1` で表示が最初から 540p60 で作られ、`CAM_mode: 1->4` と映像後の `OLED: Display off` が出ない。見る側では暗転が消え、正常な映像が約 1.2 s 早く見える。`picture at` の値自体は変わらない見込み。
- 外れたときの形: 最初の映像が出ない、または崩れる (540p60 の設定を最初から入れる順番は、作り直しのときと同じ Display_720P60_50_t だが、起動の経路で通したのは初めて)。電源を入れ直し、`cam-mode.txt` を消すか前の本体に戻す。
- 判定: ログに `cam mode: cache says 4 (16:9)` があり、`CAM_mode:` の行と映像後の `OLED: Display off` が無いこと。映像と信号。できれば動画でも暗転が無いこと。

### カメラ形式の控えの結果 (2026-10-28、boot 276〜277、本体 b337e693。276 は速い側、277 は遅い側)

- 2 回とも `cam mode: cache says 4 (16:9)`、`switch to bw:1 ... CAM_MODE=4 4:3=1`、`picture at` 940 / 977 ms。映像の後の `CAM_mode:` の行と `OLED: Display off` は 2 回とも無い (前は boot 236〜275 で毎回あった)。ユーザーも「出てから消える動き」が無くなったと確認。
- boot 276 は映像なし。リンクの行が `valid R0 L0, rssi R255/255 L255/255, stat R10 L10` で、送信機を切っていた boot 240〜245 と同じ形。受信チップの初期化 (指紋、DCOC の値、after init) は正常。送信機側の都合 (まだ送信していなかった) か、この変更の副作用 (DM5680_SetFPS(540p60) を起動の経路で最初に送る順番) かは、この 1 回では切り分けられない。277 は同じ経路で信号を受けた。
- 記録は `logs/goggle2/2026-10-28-cam-mode-cache/`。判定を固めるには、送信機が送信中と分かっている状態で数回起動して、`valid R1 L1` が毎回出ることを見る。

### カメラ形式の控えの 2 回目の確認 (2026-10-28 夜、boot 278〜280、動画 IMG_1018 / IMG_1019)

- 3 回とも映像は正常 (ユーザー確認)。278 と 279 はビープの前に電源を切ったのでアプリのログは写されていない。280 だけカードに残ったアプリのログ (`HDZGOGGLE.log`) を `boot-logs/boot280-stage304.live.log` に写した。
- boot 280: `cam mode: cache says 4 (16:9)`、`picture at 1142ms`、`valid R1 L1`、Signal accquired。`CAM_mode:` の行は無い。映像の約 1 s 後と 1.8 s 後に左キーの長押し (`btn_press`) でメニューを開いて閉じており、そのときの消灯と点灯は操作による切り替え。
- 動画 (30 fps、アイピースの明るさ、`video/eye_*.txt`): ロゴが消えてから、映像が出て消えずに続くまで。

| 動画 | 本体 | ロゴ消灯 | 安定した映像 | 間隔 |
|---|---|---|---|---|
| IMG_1017 | 7bc02ae5 (控え無し) | 4.35 s | 6.77 s (5.57 s に一度出て 6.40〜6.75 s に暗転) | 2.42 s |
| IMG_1018 | b337e693 (控えあり) | 7.48 s | 8.72 s | 1.24 s |
| IMG_1019 | b337e693 (控えあり) | 6.95 s | 8.22 s | 1.27 s |

- ロゴの消灯はアプリの起動直後の `OLED: Display off` なので、そこからの間隔は約 1.15 s 縮んだ (見込み 1.2 s)。
- 電源コネクタの青い LED が点いてから安定した映像まで: IMG_1018 は約 6.27 s (LED 2.43〜2.50 s)、IMG_1019 は約 6.09 s (LED 2.13 s)。LED が本体の電源投入と同時かは未確認。
- 3 本とも、映像の約 0.27 s 前に 1 コマだけ明るくなる (IMG_1017 は 5.30 s、1018 は 8.44 s、1019 は 7.94 s)。控えの前からあるもの。パネルの点灯 (`OLED: Display on`) から受信映像が届くまでの間と見ているが、未確認。

### 映像の約 0.27 s 前の 1 コマの明るさ (2026-10-29)

- 動画で測った値: 明るいのは 40 ms 以下 (IMG_1017 の 120 fps で 1 標本、60 fps に直して 2 コマ)。画面全体が一様に白っぽく、カメラのローリングシャッターで上下に分かれて写る。ロゴが消えてから 0.96〜0.99 s 後、安定した映像の 0.25〜0.28 s 前。3 本とも同じで、カメラ形式の控えの前からある。
- ログ (boot 277): ロゴの消灯に当たる `OLED: Display off` から、点灯の `OLED: Display on` (= `picture at`) まで 0.89 s。パネルが点くのはここだけで、その前の `oled pattern` は dispw の回収前なので点灯しない (`screen_display()` が `vdpo_timing_pending()` で戻る)。
- 推測: 明るい 1 コマは `screen_display(1)` の中で、FPGA の 0xa3 を 0x83 にしたままパネルの表示を入れてから、0xa3 を 0x03 に戻すまでの約 27 ms (`screen-google2.c` の display on の並び)。その後の暗い間は、FPGA が受信映像の層を出しているが、まだ受信映像が無い間。どちらも未確認。
- 時間の面: 明るい 1 コマ自体は待ち時間を作っていない。縮められる可能性があるのは、その後の約 0.27 s (受信のリンクが上がるまで、と見ている)。選局は切り替えの中 (`switch to bw` → `channel tuned`、37 ms) で、受信チップの初期化完了から約 0.1 s 後。

### 受信リンクの印と早めの選局 (本体 2ec402e9、2026-10-29 準備、ゴーグルでは未測定)

- 控え `~/LocalCodes/hdzero-card-staging/builds/HDZGOGGLE.2ec402e9.link-watch`。前は `HDZGOGGLE.b337e693.cam-mode-cache`。
- 常に入るもの (動作は変えない): `link: right/left valid at N ms (M ms after the tune)`、`link: first osd packet at N ms`、`link: tuned at N ms, watching`。起動時だけ、最初の選局から 20 ms ごとに有効フラグを問い合わせる (最長 1.5 s、`link watch` スレッド)。N は `picture at` と同じ基準。
- 旗 (カードの `early-tune` ファイルがあるときだけ): 受信チップの初期化が終わった直後にワーカーで選局し (`HDZero: early tune ...`)、切り替えでは `switch: channel already tuned by the tuner worker` として選局を省く。DM6302_SetChannel は PLL の校正 (0x118) をやり直すので、2 回目を呼ぶと先に選局した意味が無くなる。
- 試す順: (A) 旗なしで 2 回以上 (今の選局のままでのリンクの時刻)、(B) 旗ありで 2 回以上。判定は `link: ... valid at` の時刻 (A と B の差)、`picture at`、動画のロゴ消灯から安定した映像まで (A では 1.24〜1.27 s の見込み)。
- 見込み: B でリンクが約 0.1 s 早く上がり、切り替えの選局 37 ms も省ける。暗い間が縮むかは、リンクが映像の律速かどうか次第 (未確認)。
- 外れたときの形: B で映像が出ない、または遅れる (選局の後に何か別の設定が要る場合)。電源を入れ直し、`early-tune` を消す。

### 受信リンクの印の結果と、形式の指定を早める旗 (2026-10-29)

- 旗なし (本体 2ec402e9、boot 282〜283、どちらも遅い側。boot 281 はビープ前に電源断で記録なし)。動画 IMG_1021 = 282、IMG_1022 = 283。記録は `logs/goggle2/2026-10-29-link-watch/`。

| 起動 | 選局→有効 | 有効→パネル点灯 | 点灯→最初の OSD | 消灯→明るい 1 コマ (ログの点灯) | 消灯→映像 |
|---|---|---|---|---|---|
| 282 | 22 ms | 32 ms | 105 ms | 0.93 s (0.88 s) | 1.20 s |
| 283 | 21 ms | 36 ms | 28 ms | 0.88 s (0.85 s) | 1.15 s |

- 受信の有効フラグは選局の約 20 ms 後で、パネルを点ける前に立っている。OSD のデータも点灯の直後から届く。映像は点灯の約 0.27 s 後なので、暗い間は電波のつながり待ちではない。明るい 1 コマはログの点灯とコマの幅 (33 ms) の中で一致する。
- 早めの選局 (`early-tune`) で縮むのは切り替えの選局 37 ms だけの見込みになったので、後回し。
- 次の旗 `early-fps` (本体 `b3682604`、控え `HDZGOGGLE.b3682604.early-fps`): 受信チップの準備直後にワーカーで `DM5680_SetFPS(CAM_MODE)` を送り、切り替え (`Display_720P60_50_t`) では同じ形式なら省く (`switch: camera mode 4 already sent to the baseband`)。暗い間がベースバンドの形式切り替えの立ち上がりなら縮む。FPGA の再設定 (0x8C など) が原因なら変わらない。
- カード: 段階 308 から `early-fps` だけ置いた (`early-tune` は無し)。
- 判定: 動画で点灯 (明るい 1 コマ) から映像までの間隔 (旗なしは 0.27 s)。ログでは `early SetFPS 4` と `already sent` の行、映像、信号。
- 外れたときの形: 映像が出ない、または崩れる (起動の経路で選局より前に形式を送るのは初めて)。電源を入れ直し、`early-fps` を消す。

### early-fps の結果 (2026-10-29、boot 284〜285、本体 b3682604、動画 IMG_1023)

- 2 回とも `HDZero: early SetFPS 4 in the worker` と `switch: camera mode 4 already sent to the baseband`。映像と信号あり、指紋一致。記録は `logs/goggle2/2026-10-29-early-fps/`。
- 動画 (どちらの回かは不明): ロゴ消灯 8.39 s、明るい 1 コマ 9.30 s、映像 9.57 s。消灯→明るい 1 コマ 0.91 s、明るい 1 コマ→映像 0.27 s、消灯→映像 1.18 s。旗なし (1.15 / 1.20 s、0.27 s) と同じで、効果なし。
- 受信の有効フラグは選局の 41 / 82 ms 後 (旗なしは 21 / 22 ms)。2 回だけなので差かどうかは言えない。
- カードの `early-fps` は消した (本体はそのまま、旗が無ければ動作は 2ec402e9 と同じ)。
- 残る候補: 切り替えの `Display_720P60_50_t` の中で SetFPS 以外のもの (FPGA の 0x8C の入れ直しと 540p60 の設定、`Display_VO_SWITCH(1)` の M0 の起動 `DM6302_openM0(1)`)。以前の 720p60→540p60 の作り直し (選局はそのまま) でも点灯後に約 0.3 s 暗かったので、この関数の中のどれかと見ている (推測)。どれもパネル点灯の 50 ms 以内に行われる。縮めるには、これらを受信チップの準備直後 (今より約 0.13 s 前) に移し、パネルは今の時刻に点ける並べ替えが要る。得は最大で約 0.13 s の見込み (未確認)。

- 2026-10-29 ユーザーの判断: 明るい 1 コマは対応しない。残る候補 (FPGA の再設定、M0 の起動を前に出す並べ替え、`early-tune`) も得が小さいので当面は扱わない。

- 2026-10-29 ユーザーの判断: カメラ形式の控え (カードの `cam-mode.txt` と `cam_mode_cache_load()` / `cam_mode_cache_store()`) は大事なので残す。今後の本体にも入れ、カードの `cam-mode.txt` は消さない。

### アナログで起動するときの前倒し (2026-10-29 準備、本体 56bd3f20、ゴーグルでは未測定)

- ユーザーのストップウォッチ: スイッチから映像まで約 6.00 s、デジタルとアナログでほぼ同じ。
- boot 294 (アナログ、遅い側) のログ: パネルを 55.71 に一度点けてメニュー用の画面を出し、55.83 に消して dispw 720p60 を 170 ms 待ち、56.06 に点灯。その後 200 ms 待ってから受信機 (RTC6715) の電源、56.30 に選局。電源基準でパネル点灯 5.88 s、受信機の電源 6.08 s。デジタルの点灯は 5.53 / 5.70 s (boot 293 / 292)。
- 変更: アナログで起動するとき (`boot_source()` が AV module)、デジタルと同じ条件で画面の形式の切り替えを起動の早い段階で裏で始める (`start_display_timing_early_analog()`、PAL は 720p50)。これが回収されるまでパネルは点かないので、途中のメニュー用の画面も出なくなる見込み。内蔵の受信機なら電源も起動の早い段階で入れ (`rtc6715_power_early()`)、切り替えでは 200 ms のうち残りだけ待つ (`RTC6715 powered N ms ago, waiting M ms`)。
- 見込み: 切り替えから dispw 170 ms と電源待ち 200 ms が消え、0.3〜0.4 s 早い。
- 判定: ログで `vdpo: 720p60 started in the background` → 切り替えで `vdpo: collected`、`oled pattern` の後に `OLED: Display on` が無い、`waiting 0ms`。パネル点灯と選局の時刻 (電源基準) を boot 294 と比べる。映像。
- 外れたときの形: アナログの映像が出ない、乱れる (受信機の電源を早く入れたことによる)。前の本体 `HDZGOGGLE.375efc8a.card-folders` に戻す。

### アナログの前倒しの結果 (2026-10-29、boot 295〜297、本体 56bd3f20)

電源基準の秒 (アプリのログの時計から換算)。記録は `logs/goggle2/2026-10-29-analog-early/`。

| 起動 | 種類 | パネル点灯 | 選局 | 受信機の電源待ち |
|---|---|---|---|---|
| 294 (前、56bd3f20 の前) | 遅い側 | 5.88 (その前に 5.53 で一度点灯) | 6.11 | 200 ms |
| 295 | 遅い側 | 5.82 | 5.86 | 0 ms (965 ms 前に電源) |
| 296 | 遅い側 | 5.54 | 5.57 | 0 ms (679 ms 前) |
| 297 | 速い側 | 5.33 | 5.36 | 0 ms (684 ms 前) |

- 3 回ともアナログの映像は正常 (ユーザー確認)。途中のメニュー用の点灯は無くなり、dispw は起動の早い段階で裏で済んで切り替えでは回収だけ (`vdpo: collected 720p60`)。
- 295 だけ遅いのは、切り替えの最初の `dvr_update_vi_conf()` (/mnt/app の record.conf を 3 回書き直し、最後に `sync()`、`src/core/dvr.c:475-478`) が約 400 ms かかったため (296 は約 37 ms)。デジタルの切り替えではこれを映像の後に回している。
- 次の本体 `97af17a9` (控え `HDZGOGGLE.97af17a9.analog-viconf-late`): アナログの切り替えで `dvr_update_vi_conf()` を映像の後 (`dvr_stop_live()` の直前) に移した。判定は、`head tracker` の段階から `Set_RTC6715` までの間隔が毎回短いこと。

### record.conf を映像の後へ移した結果 (2026-10-29、boot 298〜300、本体 97af17a9)

| 起動 | 種類 | head tracker → 回収 | パネル点灯 | 選局 | record.conf |
|---|---|---|---|---|---|
| 298 | 遅い側 | 5.42 → 5.47 | 5.52 | 5.56 | 6.18 |
| 299 | 遅い側 | 5.43 → 5.48 | 5.53 | 5.56 | 6.20 |
| 300 | 速い側 | 5.21 → 5.26 | 5.31 | 5.34 | 5.96 |

- 3 回とも映像は正常 (ユーザー確認)。head tracker の段階から選局まで 0.13〜0.14 s で揃った (前は 295 で 0.44 s)。record.conf の書き込みは映像の 0.6 s 後。
- 遅い側の選局は 6.11 s (boot 294、改善前) → 5.56 s。速い側は 5.34〜5.36 s。デジタルのパネル点灯 (5.53 / 5.70 s、boot 293 / 292) と同じか早い。
- 記録は `logs/goggle2/2026-10-29-analog-viconf-late/`。

### 入口の 4 本を fork-system/ へ (2026-10-29、boot 301〜302)

- フラッシュの入口を、`fork-system/` を先に見て無ければ直下を見る版にした。書いたのはカードの `goggle2-install-rcs.sh` と `goggle2-install-hooks.sh` (どちらも fork-system/、rc-exp.sh が記録の終わりに呼ぶ)。boot 301 で書き、boot 302 から新しい入口で動いた。
  - rcS: fc9058fe → 514caff8 (v4、`goggle2-flash/rcS.v4`)。
  - /mnt/app/script/rc.sh: cb87f258 → b766e563。
  - rc.final (上書き領域): efe8f153 → f23fd7ab。
  - 書き換え前の控えはカードの `fork-system/goggle2-install-*/` (*.before) と `logs/goggle2/2026-10-29-entry-move/`。
- boot 302 の `rc-exp.log` の見出しは `from /mnt/extsd/fork-system/rc-exp.sh`、init-exp.log も early-exp も fork-system の版。直下の 4 本は同じ中身と確かめてから消した。直下は純正のものと 2 つのフォルダだけ。
- boot 302 はアナログで、選局までは boot 301 と同じ順と時刻だったが、アプリのログ (約 7 s まで) に `Signal accquired` が無い。送信機の状態か未確認。
- PhoenixCard のあと: 純正の rc.sh は直下の develop.sh しか呼ばないので、`fork-system/develop.sh` を直下に写し、`fork-system/goggle2-install-hooks/DONE` と `goggle2-install-rcs/DONE` を外して起動する。設定の書き戻し (`goggle2-restore-setting.sh`) と控え (`goggle2-backup.sh`) はリポジトリでは fork-system のパスに直したが、カードではまだ fork-deletable/ にあり動かない。

### 受信チップの旗の結果 (2026-10-29、ドローンは切ったまま、本体 953cb0c6)

| 起動 | 旗 | リセット→Init0 | DCOC | 初期化全体 |
|---|---|---|---|---|
| 273〜275、291 | なし | 約 117 | 133〜164 | 706〜724 |
| 303〜305 | dcoc-both | 110〜115 | 70〜85 | 632〜701 |
| 306〜307 | dcoc-both + dcoc-500us + reset-50ms | 68〜72 | 65〜78 | 581〜633 |

(ms) 指紋は全回一致、"no response" 無し、DCOC の値は前と同じ範囲 (下位で ±2、上位 16 bit は揺れる)。dcoc-500us の効き (見込み 4 ms) は揺れと区別できない。ドローンを切っていたので、受信の質はまだ見ていない (303〜307 の "valid 0" はそのため)。
次: ドローンを入れた 1 回の確認で、3 つの旗と `early-tune` をまとめて見る (映像が乱れないか、受信が有効になる時刻)。

### ドローンを入れた確認 (2026-10-29、boot 308〜309、旗 4 つ: dcoc-both, dcoc-500us, reset-50ms, early-tune)

- 映像の乱れは無し (ユーザー確認)。`Signal lost` 0、指紋一致、"no response" 無し。アプリのログは約 7 s で切れていて (ビープの後の分は電源断で失われる)、約 11 s の受信の強さの行は残っていないので、受信の強さは比べていない。
- 早めの選局: ワーカーで選局 (`HDZero: early tune band 0 ch 1`、38〜39 ms)、`link: tuned at` 703 / 741 ms (旗なしは 869〜914 ms)、受信の有効は選局の 22〜62 ms 後、切り替えでは `channel already tuned`。
- `picture at` 851 / 886 ms (旗なしの 291〜293 は 924〜973 ms)。電源基準のパネル点灯: 遅い側 5.62 s (291 / 292 は 5.71 / 5.70)、速い側 5.45 s (293 は 5.54)。各 1 回ずつなので幅は未確定。
- 旗 4 つはカードの fork-system/ に置いたまま。記録は `logs/goggle2/2026-10-29-tuner-flags-rf/`。

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

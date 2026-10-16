# 借り物の Goggle v1 で fork を安全に試す手順

原則: **フラッシュには何も書かない。** fork は SD カードから起動し、抜けば純正に戻る。

## 仕組み

純正 rc.sh (`mkapp/app/script/rc.sh`) は起動時に SD ルートの `develop.sh` を見つけると、
内蔵アプリの代わりにそれを実行する。この仕組みは v8.0.0 以降の全リリースにある。
fork の変更は `src/` (アプリ本体) だけで、カーネルモジュール・record・RX/VA ファームは
上流のまま。だから SD からアプリを差し替えるだけで fork の全機能を試せる。

fork が永続領域に書くのは `/mnt/app/setting.ini` だけ (設定を変えたとき)。
Stage 0 で借りた時点のコピーを SD に取り、返却前に書き戻す。

## 壊す経路は 1 つだけ

rc.sh は `develop.sh` より **前に**、SD ルートに次のファイルがあると確認なしで
フラッシュに書く (`write_flashes.sh`, `update_boot`)。

- `HDZGOGGLE_VA.bin`, `HDZGOGGLE_RX.bin` -> FPGA / 受信チップのファーム
- `HDZG_BOOT.bin`, `HDZG_OS.bin` -> ブートローダ / OS
- `HDZERO_GOGGLE-*.bin` -> メニューからの更新入力 (自動では走らないが置かない)
- `hdzgoggle-services-*` -> /mnt/app にサービスを展開

`build_goggle/out/` には `HDZERO_GOGGLE-*.bin` が生成される。**これを SD に
コピーしない。** 挿す前に必ず `check-sd.sh` を通す。

## 手順

### 0. 持ち主のゴーグルの版を知る (SD 不要)

Version 画面でアプリ版・RX・VA を読む。Goggle v1 は通常 9.5.0 (RX 75 / VA 200)。
それに合わせて fork を v9.5.0 の上に移植したものが `../hdzero-goggle-9.5`
(ブランチ `fast-switch-9.5`) にある。SD に載せるのは **こちらの** バイナリ。
`fast-switch-9.6` (9.6.0-rc1 ベース) のバイナリは 9.5.0 の `hdzero.ko` /
record と食い違うので使わない。表示された版が 9.5.0 でなければ、ここで止まって
考え直す。

### 1. SD カードは自分のものを使う

FAT32 で空のカード。持ち主のカード (DVR が入っている) は使わない。

### 2. Stage 0: 何も変えずに仕組みを確かめる

    cp utilities/sdcard-test/develop.sh.stage0 /Volumes/<SD>/develop.sh
    utilities/sdcard-test/check-sd.sh /Volumes/<SD>

挿して起動。純正と同じに動くはず。電源を切って Mac で見ると
`goggle-backup/` に `setting.ini.orig`, `version.txt`, `lsmod.txt` などがある。
`version.txt` が手順 0 の版と一致することを確かめる。

### 3. 9.5.0 ベースの fork をビルドする

ツールチェインは Linux x86_64 用なので Docker (devcontainer と同じイメージ) で
ビルドする。イメージは一度作れば使い回せる。

    cd ../hdzero-goggle-9.5
    docker build --platform=linux/x86_64 -t hdzg-dev .devcontainer   # 初回だけ
    docker run --rm --platform=linux/x86_64 \
        -v "$PWD":/src \
        -v "$PWD/../hdzero-goggle/toolchain":/src/toolchain:ro \
        -w /src hdzg-dev bash -c '
            cmake . -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_TOOLCHAIN_FILE=toolchain/share/buildroot/toolchainfile.cmake -Bbuild &&
            make -C build -j8'

成果物は `../hdzero-goggle-9.5/out/HDZGOGGLE`。`check-sd.sh` はこれと SD 上の
バイナリを比べる。

### 4. Stage 1: fork を SD から起動 (全機能 OFF のまま)

    cp utilities/sdcard-test/develop.sh.stage1 /Volumes/<SD>/develop.sh
    cp ../hdzero-goggle-9.5/out/HDZGOGGLE    /Volumes/<SD>/HDZGOGGLE
    utilities/sdcard-test/check-sd.sh /Volumes/<SD>

fork の `[speed]` `[bugfix]` の既定値は全部 false。最初の起動は純正相当の挙動 +
ログだけ。短時間で: 起動、メニュー、チャンネル切替、映像、短い DVR 録画、通常電源断。
ログは `utilities/save-goggle-logs.sh /Volumes/<SD>` で回収。

### 5. 機能を 1 つずつ ON にする

順番はリスクの低いものから。
1. UI だけのもの (fast_menu, label_diff, ui_throttle, menu_antialias_off ...)
2. DVR まわり (dvr_*_wait, dvr_defer_stop, wait_for_recording)
3. 受信チップ / バスに触るもの (spi_burst, tuner_bus, tuner_bus_duty40, short_i2c_timeout) は最後

v1 では `hardware-goggle.c` の async display / tuner standby がスタブ
(純正動作にフォールバック) なので、async_display / async_tuner の効果は出ない。
既定値でも 1 つだけハードに触るものがある: `iic_init()` の `twi2_probe()` が
SoC の TWI2 クロックレジスタを aww で書いて読み戻し、元に戻す。揮発性で
フラッシュには残らない。

### 5b. 1 枚のカードを Goggle 2 と v1 で使い回す (develop.sh.dual)

`develop.sh.dual` を develop.sh にすると、内蔵の純正アプリで機種を見分けて
起動し分ける。Goggle 2 ではそれまでの develop.sh (`develop.g2.sh` に改名して置く)
を中身そのままで実行し、v1 では `HDZGOGGLE.v1` を RAM にコピーして起動する。
`HDZGOGGLE` は Goggle 2 用のままなので、v1 で Goggle 2 用のバイナリが動くことはない。
機種は 9.6 以降なら /mnt/app/platform、9.5 系なら純正アプリの中身で見分けるので、
どちらの機種も 9.5 系・9.6 系のどちらが入っていてもよい。

    cp /Volumes/<SD>/develop.sh                    /Volumes/<SD>/develop.g2.sh   # 最初だけ
    cp utilities/sdcard-test/develop.sh.dual        /Volumes/<SD>/develop.sh
    cp build_goggle/out/HDZGOGGLE                    /Volumes/<SD>/HDZGOGGLE.v1
    mkdir -p /Volumes/<SD>/goggle-backup                                        # 原本の控えを先に入れる
    cp logs/goggle-v1/20260916-115451/goggle-backup/setting.ini.orig /Volumes/<SD>/goggle-backup/
    utilities/sdcard-test/check-sd.sh /Volumes/<SD>

check-sd.sh は両機用のとき、HDZGOGGLE.v1 が build_goggle のビルドと同一で Goggle 2 の
ドライバを含まないこと、setting.ini.orig が借りた日の原本 (5eb27b713a) と同じことも見る。
build_goggle の out/ にはファームのイメージ (HDZERO_GOGGLE-*.bin) もできるが、借り物の
v1 のカードには絶対に置かない (置くと更新が始まる)。
v1 の新しいプレーヤの確認は `autotest.v1.txt` を autotest.txt としてカードに置く。

### 5c. Goggle 2 の設定とフラッシュを控える (緊急更新の前に)

自分の Goggle 2 で緊急更新 (`HDZG_OS.bin`) を試す前に取る。緊急更新は設定のある
アプリ領域ごと書き直すので、設定はここでしか残らない。

    cp utilities/sdcard-test/develop.sh.dual     /Volumes/<SD>/develop.sh
    cp utilities/sdcard-test/goggle2-backup.sh   /Volumes/<SD>/
    utilities/sdcard-test/check-sd.sh /Volumes/<SD>

Goggle 2 で一度起動し、映像が出るまで待つ (初回だけ数十秒長い)。カードの
`goggle2-backup/` に setting.ini、/mnt/app の tar、NOR 全体 (mtd0〜7) の写しが
できる。読むだけで、v1 では動かない。Mac で確かめて `logs/goggle2/` に移す:

    utilities/sdcard-test/goggle2-backup-check.sh /Volumes/<SD>

二度目からの起動は setting.ini.last-boot を取り直すだけ。取り直すときは
`goggle2-backup/` を消す。

緊急更新の後は設定が初期値に戻る。控えの setting.ini を一度だけ書き戻す:

    mkdir -p /Volumes/<SD>/goggle2-restore
    cp logs/goggle2/<日付>-flash-backup/setting.ini /Volumes/<SD>/goggle2-restore/
    md5 -q /Volumes/<SD>/goggle2-restore/setting.ini > /Volumes/<SD>/goggle2-restore/setting.ini.md5
    cp utilities/sdcard-test/goggle2-restore-setting.sh /Volumes/<SD>/

書くのは /mnt/app/setting.ini だけ。結果は `goggle2-restore/result.txt`。

### 5d. Goggle 2 の起動時間の実験 (自分の Goggle 2 だけ)

フラッシュの 3 か所を変えてある (2026-10-16、17)。カードに実験用の
スクリプトが無いか、カードを抜けば、rcS のクロックの固定 (15 秒で戻る) と
印のほかは純正どおりに動く。足した後のファイルと元のファイルは
`goggle2-flash/` にある。

| 場所 | 入口 | カードのスクリプト |
|---|---|---|
| アプリ領域 `/mnt/app/script/rc.sh` (先頭) | `rc-exp.sh` があれば rc.sh の代わりに実行。0 以外で終われば純正を続ける | `rc-exp.sh` |
| 上書き領域 `/etc/init.d/rc.final` (S スクリプトの繰り返し) | カードのマウント後 (S01app より後) の S スクリプトごとに `init-exp.sh <パス>` を呼ぶ。0 なら済んだもの。各 S スクリプトの前に dmesg へ `bootmark:` の印を書く | `init-exp.sh` |
| 上書き領域 `/etc/init.d/rcS` (新規、純正の写し) | 先頭で CPU の最低クロックを最高に固定し、15 秒後に戻す。各段の前に `bootmark:` の印 | — |

元に戻すとき、rc.final と rcS は上書き領域のファイル
`/overlay/upperdir/etc/init.d/rc.final`、`.../rcS` を消す。`/etc/init.d/` の
側を消してはいけない (元のファイルまで隠れて起動しなくなる)。rc.sh は
`goggle2-flash/rc.sh.orig` を書き戻す。どちらも起動しなくなったら SD からの
緊急更新 (`HDZG_OS.bin` ほか 3 個) で全部戻る。

入口を入れ直すとき (PhoenixCard や緊急更新のあと) は、SSH で書くか、カードに
`goggle2-install-hooks.sh` と `goggle2-flash/rc.sh`、`goggle2-flash/rc.final` を
置いて一度起動する。develop.sh.dual がアプリの前に呼び、いまの中身が純正か
入口入りのときだけ書く。結果は `goggle2-install-hooks/result.txt`。

分かったこと:
- **I2C port 1 (IT66121, MCP3021, BMI270, NCT75) に、電源投入から早く触ると
  固まり、電源を切るまで戻らない。** 4.3 秒と 6.2 秒は失敗、7.6 秒は成功。
  純正の S50usb の `sleep 2` と rc.sh の `sleep 1` がこの待ちを兼ねていた。
  準備ができたかを通信で確かめると、それ自体で固まるので、決まった時刻まで
  待つ (`rc-exp.sh` の `I2C_NOT_BEFORE`)。
- rc.sh の `dispw` (画面の設定) は外せない。外すと fork は lvgl_init で落ち、
  純正も止まる。裏で走らせて、アプリの前に終わりを待つのはよい。
- S50usb (ADB とカメラ機能の USB の準備) は要らない。Goggle 2 には USB の口が無い。
- S10udev は udevd を裏で起動するだけでよい (一斉認識の trigger と settle は
  省ける)。規則が作るリンクのうち要るのは `/dev/rtc` だけで、`init-exp.sh` が作る。
- アプリと録画プロセスは同じ共有ライブラリ (約 8MB) を使う。I2C を待つ間に
  先読みすると (`preload.goggle2.txt` をカードの `preload.txt` に)、アプリの
  起動が速くなる。
- 画面のモードの切り替え (`dispw -s vdpo ...`) に毎回 1 秒かかるのは、
  disp.ko の disp_vdpo_disable() の最後にある 1000 ms の待ち。disp.ko は
  `dispw -x` が /mnt/app/ko から insmod するだけなので、待ちを 100 ms に
  書き換えた写し (`goggle2-ko/disp.ko`) をカードの `ko/` に置けば、
  rc-exp.sh と init-exp.sh の `dispw_x` がそれを入れる。フラッシュは触らない。
- 起動の揺れ (同じ設定で 0.5 秒) は CPU のクロック。ondemand は負荷を 2 秒に
  1 回しか見ず、多くの回は 816 MHz のまま起動する。rcS の写しの先頭と
  `init-exp.sh` (S10udev) で最低クロックを最高に固定し、`rc-exp.sh` が記録の後に戻す。
- OSD のフォント (BTFL で 4.3MB) は、カードの `resource/OSD/FC/` にあれば
  フラッシュより先にそこから読まれる。純正の `BTFL_000.bmp` と
  `BTFL_FHD_000.bmp` を同じ中身のまま置くと、読み込みが 0.8 秒から 0.4 秒になり、
  OSD の組み立てでの待ち (ときどき 0.2〜0.4 秒) が無くなる。
- `app-sampler.sh` は測るときは `.off` にしておく (動いていると受信チップの
  初期化が延びる)。`check-sd.sh` が NG にする。

起動を測るカード (HDZERO_BOOT、32GB、FAT32 の 64KB クラスタ) に置くもの:
`rc-exp.sh`、`init-exp.sh`、`develop.sh` (= develop.sh.dual)、`develop.g2.sh`
(= develop.sh.vopatch)、`goggle2-*.sh`、`wifi-hello.sh` と `wifi-hello.txt`、
`HDZGOGGLE` と `HDZGOGGLE.features` (`i2c_gate`)、`stage.txt`、`preload.txt`、
`ko/disp.ko`、`g2lib/` (47 本と links.txt)、`goggle2-flash/`、
`resource/OSD/FC/BTFL_000.bmp` と `BTFL_FHD_000.bmp` (ゴーグルの /mnt/app から写す)。
起動ごとの記録は `boot-logs/` (アプリのログ、dmesg、カードより前の dmesg と
クロックの履歴 `early-stage<段階>.txt`)。

時刻はカードの `init-exp.log` と `rc-exp.log` に残る。カードの起動スクリプトの
補助 (`wifi-hello.sh`) で Wi-Fi の子機モードから SSH で入れる (ゴーグルは
Mac の ARP の問い合わせに応えないので、ゴーグルから Mac へ ping を出す)。

### 6. 返却前

    cp utilities/sdcard-test/develop.sh.restore /Volumes/<SD>/develop.sh
    rm /Volumes/<SD>/HDZGOGGLE
    utilities/sdcard-test/check-sd.sh /Volumes/<SD>

一度起動して電源断 (setting.ini が借りた時点に戻る)。SD を抜き、
**SD なしで** もう一度起動して純正が動き、設定が持ち主のものであることを確認。
`goggle-backup/` は手元に残しておく。

## 途中で固まったら

電源を切り、SD を抜き、電源を入れる。それで純正に戻る。フラッシュに触っていない
ので、他に復旧すべきものは無い。

## やらないこと

- Firmware メニューからの更新、VTX 更新
- `.bin` を SD ルートに置く
- Storage メニューの SD フォーマット / 修復 (ゴーグルは壊さないがテストが濁る)
- 持ち主のカードの使用

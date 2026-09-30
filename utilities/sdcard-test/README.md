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

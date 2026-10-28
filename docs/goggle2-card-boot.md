# Goggle 2: カードから起動してカーネルと DT を試す (段階 2 の下調べ、2026-10-20)

指示書 `docs/goggle2-handoff.md` の段階 2-a の答え。ゴーグルには触っていない。
読んだのは、公開 SDK のソース (lindenis-org)、復旧イメージ HDZG_OS.bin の中身、
純正 u-boot の文字列、純正 rootfs のスクリプト、2026-10-15 の NOR の控え。

## 結論

**NOR に 1 バイトも書かずに、カード上のカーネルと DT で起動する道がある (見込み)。**
BROM → カードの boot0 (sector 16) → カードの u-boot (sector 32800、HDZG_OS.bin の
boot_package.fex そのまま、work_mode は 0 = 普通の起動) → カードの env 区画 (私たちが
書く) → カードの boot 区画のカーネル → bootargs は NOR のものを文字列で固定
(root=/dev/mtdblock2、partitions=...mtdblock...) → 以後は今の起動と同じ。
失敗の形は「真っ暗で止まる」で、カードを抜いて電源を入れ直せば NOR から今までどおり起動する。

まだ確かめていないことは「残る未確認」に書いた。試すのは段階 2-b (ユーザーの了承の後)。

## 答え (2-a)

### (1) カードから起動した u-boot は環境変数をどこから読むか

起動した媒体の **「env」という名前の区画** から読む。`common/env_sunxi_flash.c:137-146`
(`sunxi_partition_get_offset_byname("env")` → `sunxi_flash_read`)。見つからない、または
CRC が合わないときは、u-boot に埋め込まれた既定の環境を使う (`use_default`)。
純正の 2 つの u-boot (NOR 用、カード用) に、この経路の文字列
(`fail to find part named`、`used mbr [%d]`、`crc mbr copy`) がどちらも 3 つずつある。

カード起動では `sunxi_flash` の読み書きがカードに向き、論理アドレスに
20 MB (`CONFIG_MMC_LOGICAL_OFFSET` = 40960 セクタ、`include/configs/sun8iw16p1.h:409`、
`drivers/sunxi_flash/sdmmc.c:26`) が足される。区画表 (sunxi MBR) は論理 0 から 4 つ
(`SUNXI_MBR_COPY_NUM` 4、16 KB ずつ) を順に読み、magic `softw411` と crc32 が合った
最初のものを使う (`board/sunxi/common/sys_partition.c:213-240`)。

環境の形は u-boot 標準 (先頭 4 バイトの crc32 + 128 KB)。HDZG_OS.bin の env_nor.fex で
crc を計算して一致を確かめた。

### (2) 製品モード (NOR を書く) に入る条件

u-boot ヘッダの `boot_data.work_mode` が `WORK_MODE_CARD_PRODUCT` (0x11) のとき
(`include/spare_head.h:34`、`common/cmd_sunxi_sprite.c:41`、
`drivers/sunxi_flash/sunxi_flash.c:452-458`)。この欄は u-boot バイナリの先頭から
**0xe4** にある (`include/private_uboot.h` の struct を足し算: 0x30 + dram_para 128 +
run_clock 4 + run_core_vol 4 + uart_port 4 + uart_gpio 16 + twi_port 4 + twi_gpio 16)。

HDZG_OS.bin に入っている u-boot (カード用 1,671,168 B、NOR 用 376,832 B) は、
どちらも **work_mode 0 (普通の起動)、storage_type 0**。boot0 はカードから読んだとき
storage_type だけを 1 (SD) にする (SDK の旧 boot0、
`basic_loader/boot0/load_boot1_from_sdmmc/load_boot1_from_sdmmc.c:107-116`)。
製品モードの 0x11 は PhoenixCard がカードに書くときに入れていると推測する
(未確認。確かめるには PhoenixCard で作ったカードの sector 32800 + 0x25400 + 0xe4 を読む)。

つまり、boot_package.fex をそのまま dd したカードの u-boot は普通の起動モードで動く見込み。

### (3) sprite_update は何を見て何を書くか

HDZero 独自の命令で、ソースは無い。文字列から分かること:
- **NOR 用 u-boot にだけある。** 命令名 `sprite_update`、説明
  `update firmware from sdcard for hdzero`、その先に `sunxi card update hdzero`、
  `HDZG_OS.bin`、`update firmware success`。カードの FAT の HDZG_OS.bin を見て NOR を
  書く経路 (公式の Method 2)。
- **カード用 u-boot には `sprite_update` 命令が無い。** ある命令: boota bootm fatload
  fatls fatinfo go if md mw printenv reset run saveenv setenv sleep sprite_test
  sunxi_bmp_info sunxi_flash usb。既定の環境は NOR 用と同じ
  `bootcmd=run hdzero_update setargs_nor boot_normal`、`hdzero_update=sprite_update`。
  → カードの env が読めないと `run` が `sprite_update` で失敗し、u-boot のプロンプトで
  止まる (画面は真っ暗)。NOR は書かれない。

### (4) PhoenixCard が作るカードの配置

HDZG_OS.bin (IMAGEWTY 0x300、暗号化なし、34 項目) の cardscript.fex:
`mode=product`、boot0_sdcard.fex を sector 16、boot_package.fex を sector 32800、
`[card_boot] start = 40960`。sys_partition.fex の区画: boot 6528、rootfs 23808、
overlay 1024、env 256、bootlogo 512、app 24576 セクタ、UDISK 残り。
sunxi_mbr_nor.fex を復号したら、NOR では boot が論理 32 セクタから
(MBR 1 つ分の後)、以後が連続。crc32 (version 以降 16380 バイト) の計算が一致。

boot_package.fex は `sunxi-package` 形式で 5 項目 (scp、optee、u-boot、soc-cfg、dtb)。
ヘッダの add_sum は「add_sum 欄を 0x5F0A6C39 に置いて全体の 32 bit 和」で、
NOR 用、カード用の両方で一致を確かめた。boot0 (eGON.BT0) の check_sum も同じ方式で一致。
→ dtb を入れ替えた package を自分で作れる。

scp、optee、soc-cfg は NOR 用とカード用で同一。**dtb は内容が同一** (dtc で両方を
復号して diff が空。カード用は 94,720 B と大きいが詰め物の差)。NOR の 2026-10-15 の
控えの DTB (mtd0 の 0x9d000) は復旧イメージの NOR 用 dtb と md5 が同じ。

boot0_sdcard.fex、boot0_spinor.fex、NOR の boot0 の dram_para は先頭 6 語が同じ。

### (5) cmdline の `partitions=` はどこから来るか

u-boot が起動のたびに区画表から作り、環境に `setenv("partitions", ...)` する
(`board/sunxi/common/board_common.c:255-305`、`sunxi_fastboot_init`)。媒体が NOR なら
`mtdblock<n+1>`、SD なら mmcblk0p の番号 (`__set_part_name_for_sdmmc`)。
環境区画には保存されない (fw_printenv に無く cmdline にあるのはこのため)。

Linux 側で使うのは 2 か所:
- `/pseudo_init` の `set_parts_by_name` (`/dev/by-name/*` を作る)。
- `/etc/preinit` の `get_overlay_part` (`overlay@...` から上書き領域の装置を決めて
  jffs2 でマウントし pivot_root)。

→ カード起動で NOR の rootfs を使うには、bootargs の `partitions=` を NOR の文字列で
**固定** して渡す (`${partitions}` を使わない)。

### (6) カーネルは NOR の区画をどう作るか

`drivers/mtd/sunxipart.c`: NOR の (1024-16) KB にある sunxi MBR を読んで mtd 区画を作る
(sun8iw16 の `MBR_OFFSET`)。パーサは `drivers/mtd/mtdpart.c:789` の既定一覧に入っている。
cmdline には依らないので、カードから起動したカーネルでも DT が同じなら
`/dev/mtdblock1..7` ができ、`root=/dev/mtdblock2` が使える。

### (7) 2026-10-19 に止まった原因の候補 (推測、根拠つき)

SDK の u-boot は普通の起動のたびに `update_bootcmd()` を呼び
(`board_common.c:1229-1246` の `board_late_init`)、そこで
**`char boot_commond[128]; strcpy(boot_commond, getenv("bootcmd"));`**
(`board_common.c:528-537`) → 書き換えて `setenv("bootcmd", boot_commond)` (同 607 行)。
2026-10-19 に書いた bootcmd は **181 文字** (元は 41 文字) で、**54 バイトはみ出す**。
スタックが壊れて止まる。カードの有無に関わらず止まったことと合う。
純正 u-boot の文字列に `base bootcmd=%s`、`bootcmd set setargs_mmc`、`to be run cmd=%s`
があり、この関数は HDZero の u-boot にもある。配列の大きさが同じかは確かめられない。

もう 1 つの疑い「hush が失敗した命令の後で止まる」は **外れ**:
`common/cli_hush.c:1880-1898` は失敗を last_return_code に入れて次の命令に進む
(`&&`/`||` のときだけ飛ばす)。ただし `run a b c` は最初に失敗した変数で止まる
(`common/cli.c:112-132`)。

**教訓:** bootcmd は 127 バイト以内。長い内容は別の変数 (setargs_* など) に置く。

### (8) 作り直したカーネルが約 41 万バイト大きかった理由

**CONFIG_KALLSYMS_ALL。** #69 の /proc/kallsyms は t/T/W (関数) だけ (29,278 行)、
作ったカーネルの System.map には r/d/b も載っていた。KALLSYMS_ALL を外して作り直した
config-hdzlike5 (`logs/goggle2/2026-10-19-kernel-probe/kbuild5.sh`):

| | Image | zImage |
|---|---|---|
| 純正 #69 | 4,495,944 | 2,939,232 |
| hdzlike4 (KALLSYMS_ALL=y) | 4,913,016 | |
| hdzlike5 (KALLSYMS_ALL=n) | 4,544,376 | 2,969,072 |

残る差 48,432 B は .text の差 (約 17 KB) と、こちらにだけある関数 (218 個) の分と見る (推測)。
image は Docker ボリューム lindenis-kernel の /k/out5。

### (9) ついでに分かったこと

- **UART0 は PH7/PH8** (u-boot ヘッダの uart_port 0、uart_gpio = port 8 番 7/8、機能 3)。
  メモリの「根拠なし」が埋まった。基板上の位置は未確認のまま。
- u-boot ヘッダの run_clock は 912 MHz、core 1200 mV。sys_config の `[target] boot_clock = 912`、
  `storage_type = 3` (NOR)。カーネルが 816/1104 MHz のどちらで始まるかとの関係は未確認。
- sys_config に `[auto_update]`、`[recovery_key]`、`[fastboot_key]`、`[key_boot_recovery]`
  は無い。`[card0_boot_para]` は PF0-PF5、4 線、high speed。
- 復旧イメージの env は `appOta=2` (初回に /mnt/app/install.sh を走らせる)。
  2026-10-15 の NOR の控えは `appOta=0`。

## 2-b の結果 (2026-10-20、boot 500〜505、カード HDZCARD = 元 NO NAME2)

**カードから起動した。NOR には触っていない。** 6 回とも `/proc/cmdline` に `hdzcard=1`、uname は #69。
アプリのログに異常なし (crash 無し、"no response" 無し)。記録は `logs/goggle2/2026-10-20-card-boot/`。

| 起動 | kernel end | rcS | 起動 (launch) | main (電源基準) | 映像 (電源基準) | 816 MHz の tick |
|---|---|---|---|---|---|---|
| 500 | 1.18 | 1.86 | 3.42 | 5.967 | 7.085 | 103 |
| 501 | 1.20 | 1.83 | 3.31 | 5.838 | 6.956 | 98 |
| 502 | 1.17 | 1.82 | 3.35 | 5.925 | 7.112 | 99 |
| 503 | 1.19 | 1.83 | 3.35 | 5.913 | 7.043 | 98 |
| 504 | 1.16 | 1.81 | 3.29 | 5.888 | 6.971 | 100 |
| 505 | 1.15 | 1.79 | 3.37 | 5.961 | 7.066 | 100 |

- カーネル起動からの時刻は NOR 起動 (boot 158: kernel end 1.20、rcS 1.86、launch 3.28、main→映像 1129 ms) と同じ。
- **電源からカーネル起動までが約 1.05 秒長い。** アプリのログの時計の切り替わりの時刻と
  i2c の関門の行の uptime から出した「電源基準 − uptime」は、カード起動で 2.31〜2.37 s、
  NOR 起動 (boot 158、同じ方法) で 1.28 s。boot0 がカードから 1.98 MB の package を読む、
  カード用 u-boot の初期化、u-boot がカードからカーネルを読む、のどれかで、UART が無いので内訳は不明。
- 全部「遅い側の回」(816 MHz が約 1.0 s)。速い側の回は出なかった。
- カードを使う目的 (カーネルと DT の試験) には、この 1 秒は定数の差として扱う。
  比べるのはカーネル起動からの時刻と、同じカードの上での前後。

### 新しいカーネルを載せる前に分かった危険 (2026-10-20)

NOR は w25q256 (0xef4019、32 MB) で、純正カーネルの `/proc/mtd` は **消去ブロック 64 KB**。
SDK の `drivers/mtd/spi-nor/spi-nor.c` の w25q256 の項目には SECT_4K が付いていて、
config-hdzlike4/5 は `CONFIG_MTD_SPI_NOR_USE_4K_SECTORS=y`。そのまま起動すると mtd の消去ブロックが
4 KB になり、64 KB ブロックで書かれた jffs2 (overlay、app、UDISK) を preinit が rw でマウントした時点で
壊す見込み (ブートローダは無事なので文鎮ではないが、PhoenixCard で戻して設定も入れ直す羽目になる)。
→ **hdzlike5 は載せない。** `-d MTD_SPI_NOR_USE_4K_SECTORS` にした hdzlike6 を作る。
教訓: 新しいカーネルを NOR の jffs2 に触らせる前に、消去ブロックの大きさと 4 バイトアドレスの扱いが
純正と同じかを config と表で確かめる。

### hdzlike6 の起動で NOR が壊れた (2026-10-20 夜)

hdzlike6 (KALLSYMS_ALL と 4K セクタを外したもの) をカードから起動したところ、起動画面 (ロゴ) で止まり、
その後 **カード無しでもロゴで止まる** ようになった。ロゴが出るので boot0、u-boot、bootlogo 区画は生きている。
カーネル、rootfs、app のどれが壊れたかは読むまで分からない。
原因は未確認だが、純正カーネルの spi-nor (SFDP 対応の新しい版) と SDK の spi-nor の違いで NOR の読みが
狂い、jffs2 がマウント時に「未フォーマット」と見たブロックを消去した、または rcS や preinit の
「マウント失敗なら mkfs して dd」が走った、と推測する。「普通の起動では NOR に書かない」は誤りだった。

**教訓:** 作ったカーネルは、mtd の読み出しが正しいと確かめるまで NOR を rw でマウントさせない。
確かめ方は、NOR を触らない initramfs で起動して mtd1〜7 の md5 を復旧イメージと比べる、など。

復旧の順: (1) カードに純正カーネルの像を戻して 1 回起動し、どこまで生きているかを見る。
(2) だめなら PhoenixCard で全体を戻し、設定 (goggle2-restore、md5 205892861a)、入口 (goggle2-install-hooks.sh)、
rcS v3 (SSH) を入れ直す。

## 次の手 (2026-10-20 夜に立てた計画)

### 規則

**新しいカーネルは、NOR の読みと書きが純正と同じだと証明するまで、NOR を rw でマウントさせない。**
jffs2 はマウント時に「未フォーマット」と読んだブロックを消去し、rcS と preinit と rc.preboot はマウントに
失敗すると mkfs や dd を走らせる。ユーザー空間が書かなくても NOR は書かれる。

### 手 1: 検証用の仕組み (Mac だけで作れる。ゴーグルは起動 2 回)

| 部品 | 中身 | 根拠 |
|---|---|---|
| 検証カーネル | hdzlike6 に initramfs の対応 (CONFIG_BLK_DEV_INITRD、RD_GZIP) を足したもの | 純正 #69 に initramfs の対応は無い (kallsyms に populate_rootfs が無い)。hdzlike の config にも無い |
| initramfs | 純正 rootfs の busybox (musl、動的) と lib/libc.so と /init。/init は mtd0〜7 を読んで md5、/proc/mtd、dmesg をカードに書き、ビープ 3 回で知らせて電源を切る。NOR は一切マウントしない | 書くのはカードだけ |
| カードの像 | boot 区画に検証カーネル、ramdisk 区画を足し、env の boot_normal を `sunxi_flash read ... boot; sunxi_flash read ... ramdisk; bootm <kernel> <ramdisk>`、bootargs は `rdinit=/init` で root= 無し | make-card-boot-image.py に区画と env の指定を足す |

判定:
1. 読みだけの起動 1 回。mtd1 (1b65ffaf)、mtd2 (31dae0e5)、mtd5 (6bf83182)、mtd0 の package (d0b4b3e4) と
   boot0 (純正の読みで 67ff3970)、16 MB より上の app と UDISK (純正の nor-check の値) と比べる。
   全部一致 → 読みは正しい。16 MB より上だけ違う → 4 バイトアドレスの扱い。全部違う → 読み方式かクロック。
   直すのは SDK の drivers/mtd/spi-nor/spi-nor.c か drivers/spi/spi-sunxi.c で、同じ仕組みで再検証。
2. 読みが通ったら、UDISK (28 MB より上、中身は不要) にだけ消去と書き込みの試験 (了承を得てから)。
   読み戻しと、mtd0〜6 が変わっていないことを確かめる。
3. 2 つ通ったものだけ rootfs で起動する。その前に復旧カードの md5 (layout-phoenix-image.txt) と設定の控えを確かめる。

### 手 2: 正しいソースを手に入れる (並行)

純正のカーネル (Linux 4.9.118 #69) と u-boot は GPLv2 で、HDZero (Divimath, Inc.) は
HDZEROGOGGLE2_Rev20250715.zip (Recovery/HDZG_OS.bin に boot.fex = カーネル、u-boot.fex) をバイナリで配布している。
GPLv2 第 3 条は、バイナリを配布する者に「対応する完全なソース」を付けるか (3a)、3 年有効の書面の申し出を
付けるか (3b) を求める。「完全なソース」には「コンパイルとインストールを制御するスクリプト」(= config) も含む。
HDZero 自身の .ko (hdzero.ko、disp.ko、vdpo.ko、mcp3021.ko、nct75.ko ほか) も `license=GPL` / `GPL v2` を宣言している
(2026-10-15 の /mnt/app の控えの文字列)。

根拠のリンク:
- GPLv2 本文 (第 3 条): https://www.gnu.org/licenses/old-licenses/gpl-2.0.html 。同じ文面はカーネルの COPYING
  (手元の SDK: `~/LocalCodes/lindenis-v536-lichee-linux-4.9/COPYING`) と u-boot の `Licenses/gpl-2.0.txt`。
- カーネルのライセンス規則 (GPL-2.0 only): https://www.kernel.org/doc/html/latest/process/license-rules.html
- u-boot のライセンス: `u-boot-2014.07/Licenses/README` (GPLv2、一部 GPLv2+)。
- GPL FAQ (バイナリ配布とソース提供): https://www.gnu.org/licenses/gpl-faq.html (2026-10-20 は 429 で取得できず、
  該当項目の確認は未済)。
- HDZero の配布元: https://www.hd-zero.com/document (Rev20250715 の zip。ソースや GPL の記載は無し。
  フッターは Divimath, Inc.)。アプリの公開リポジトリ https://github.com/hd-zero/hdzero-goggle2 (MIT) には
  カーネルと u-boot のソースは無い。

請求先の候補: hd-zero.com の Contact、GitHub の hd-zero/hdzero-goggle2 の Issue、公式 Discord。
法律の助言ではない。請求するかはユーザーが決める。

### 手 3: カーネルを替えずにできること

- CPU クロックの遅い側の回 (0.3 s): 純正の cpufreq は DT の OPP 表と ondemand (SDK の
  drivers/cpufreq/sunxi-cpufreq.c)。816/1104 に分かれる理由をドライバと DT で突き止める。DT は u-boot の package の
  中なので、カード経由なら純正カーネルのまま DT だけ替えて試せる (NOR に触らない)。
- ion の 512 MB (195 ms): DRAM 1 GB のうち 512 MB が映像用 (純正 dmesg: carveout base 0x60000000 size 0x20000000)。
  使用量を debugfs で読んでから判断。
- 指示書の A1、A2 (dispw を rcS から、vdpo.ko をカードから)、F (チューナー初期化の読み直し) はカードだけの変更。

### 順番

1. 今すぐ: 検証用の仕組みを作る (Mac)。GPL の請求を出すかを決める。
2. 在席のとき: A1、A2 (各 1 回の起動)。
3. 仕組みができたら: 検証カーネルで読みの試験 1 回。結果で spi-nor を直すか、ソース待ちかを決める。
4. その後: DT のクロックの試験 (カード経由、純正カーネル)。

見込み: カーネル側 0.4〜0.8 s、クロック 0.3 s (遅い側のみ)、カードだけの手 0.1〜0.2 s。すべて見込み。

## 乾いた試しの設計 (段階 2-b、2026-10-20 に実施、上の結果のとおり)

### カードの配置 (予備カード、HDZERO_BOOT ではない)

| セクタ | 中身 | 出どころ |
|---|---|---|
| 0 | DOS MBR。区画 1 つ (type 0x0C、開始 81920 = 40 MB、終わりまで) | 自作 |
| 16 〜 127 | boot0_sdcard.fex (57,344 B) | HDZG_OS.bin |
| 32800 〜 36671 | boot_package.fex (1,982,464 B)。u-boot の work_mode 0 を確かめて置く | HDZG_OS.bin そのまま |
| 40960 〜 41087 (論理 0〜127) | sunxi MBR 4 部 (copy=4、index 0〜3、crc32)。区画 boot、env | 自作 |
| 論理 128 〜 6655 | 区画 boot (6528 セクタ): カーネル uImage。最初は純正 boot.fex (= 今動いている #69) | HDZG_OS.bin |
| 論理 6656 〜 6911 | 区画 env (256 セクタ): 下の環境 (crc32 + 128 KB) | 自作 |
| 81920 〜 | FAT32 (64 KB クラスタ)。HDZERO_BOOT と同じスクリプト一式。更新ファイルは置かない | 写し |

論理の終わり 6912 = セクタ 47872 (23.4 MB) で、FAT の 40 MB より手前。

### カードの env

```
bootcmd=run setargs_card boot_normal
setargs_card=setenv bootargs earlyprintk=sunxi-uart,0x05000c00 initcall_debug=0 console=ttyS0,115200 loglevel=1 root=/dev/mtdblock2 init=/pseudo_init partitions=boot@mtdblock1:rootfs@mtdblock2:overlay@mtdblock3:env@mtdblock4:bootlogo@mtdblock5:app@mtdblock6:UDISK@mtdblock7 cma=32M ion_carveout_list=512M@1024M hdzcard=1
boot_normal=sunxi_flash read 45000000 boot;bootm 45000000
bootdelay=0
```

- bootcmd は 28 文字 (128 の制限の中)。`setargs_nand` を含まないので `update_bootcmd` の
  置換を受けない。misc 区画は無いので `set_bootcmd_from_misc` も変えない (SDK の読み)。
- `hdzero_update` を呼ばない。`sunxi_flash read ... boot` はカードの boot 区画から、
  uImage のヘッダで大きさを決めて読む (`common/cmd_sunxi_flash.c:60-110`)。
- `hdzcard=1` が印。Linux で `/proc/cmdline` に出る。

### 成功の判定

`/proc/cmdline` に `hdzcard=1`、`uname -v` が `#69`、以後の起動 (rc-exp.log、映像) が
今までと同じ。これで「カードの boot0/u-boot/env/カーネルで起動し、rootfs と app は NOR」
が言える。

### 外れたときの形と戻し方

- 真っ暗で止まる (boot0 がカードを受け付けない、env が読めない、カーネルが読めない)
  → カードを抜いて電源を入れ直す。NOR はそのまま。
- 起動はするが動きが違う → 電源を切り、カードを抜く。

### NOR に書かれない根拠

1. u-boot の work_mode は 0 (ヘッダ 0xe4 を書く前に確かめる)。製品モードの経路
   (`sunxi_card_sprite_main`) は 0x11 のときだけ。
2. 普通の起動で媒体が SD のとき、`sunxi_flash` の読み書きはカードに向く
   (`sunxi_flash_boot_handle` → `sdmmc_init_for_boot`)。NOR のドライバは初期化されない。
3. bootcmd が `hdzero_update` を呼ばない。カード用 u-boot にはその命令自体が無い。
4. FAT に更新ファイル (HDZG_OS.bin など) を置かない。フックが止める。

### 残る未確認

- V536 の実物の boot0_sdcard (ソースは SDK に無い。旧い boot0 のソースと、PhoenixCard の
  カードで 2 回起動した事実から推している)。
- HDZero が u-boot に加えた変更のうち、文字列に出ないもの。
- カード用 u-boot が起動時に bootlogo 区画が無いことをどう扱うか (エラー表示だけと推測)。
- u-boot の 128 バイトの配列が HDZero の版でも同じか。

### 必要なもの

- 予備の microSD (HDZERO_BOOT、G2RECOVERY 以外)。
- Mac で生の装置への dd (sudo はユーザーが `!` で行う)。
- 在席 (電源の入れ直し)。PhoenixCard の復旧カードは念のため手元に。

### 2-b の後 (2-c) ※ 2026-10-20 夜の失敗の後は「次の手」の規則に従う (検証用の仕組みを先に)

- カーネルを hdzlike6 以降に替えて同じ判定 (検証を通したものだけ)。
- 1 つずつ: 起動時の governor を performance に (遅い側の回の 0.3 s)、USB ホスト外し
  (103 ms)、mmc の probe の非同期化 (333 ms)。見込みは合わせて 0.4〜0.8 s。
- DT は package の dtb を入れ替えて add_sum を直す (方式は確認済み)。
- NOR に書くかは、何回も通ったものについて別に判断する。

## 道具と場所

- `utilities/goggle2-image/imagewty-unpack.py`: IMAGEWTY の一覧と展開 (読むだけ)。
- `~/LocalCodes/hdzg-os-unpacked/`: items/ (HDZG_OS.bin の 34 項目と package の各項目、
  dtb)、rootfs/ (純正 rootfs の展開。device node 以外)、nor.dts、card.dts。
- `~/LocalCodes/lindenis-v536-lichee-brandy/` (u-boot 2014.07 + 旧 boot0)、
  `~/LocalCodes/lindenis-v536-device/` (V536 の設定と boot0/u-boot のビルド済み)。
  どちらも 2026-10-20 に shallow clone。
- `logs/goggle2/2026-10-19-kernel-probe/`: kbuild5.sh、System.map-hdzlike5、config-hdzlike5。

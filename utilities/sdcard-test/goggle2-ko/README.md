# goggle2-ko: カードから入れる、書き換えたカーネルモジュール

Goggle 2 の起動時間の実験で、フラッシュに書かずにカーネルモジュールを差し替える
ためのもの。カードの `ko/` に置くと、rc-exp.sh と init-exp.sh の `dispw_x` が
純正の `dispw -x` の代わりに insmod する。消せば純正に戻る。

## disp.ko

書き換えた disp.ko はリポジトリに置かない (GPL のモジュールで、元のソースが
公開されていないため、改変したバイナリを配ると対応するソースを出せない)。
`make-disp-ko.sh` が純正の disp.ko に 4 バイトのパッチを当てて、このフォルダの
`disp.ko` (.gitignore 済み) を作る。

    utilities/sdcard-test/goggle2-ko/make-disp-ko.sh            # 本家がリポジトリに入れている純正から
    utilities/sdcard-test/goggle2-ko/make-disp-ko.sh <純正の disp.ko> [出力先] [待ち ms]

入力は md5 と書き換える 4 バイトを確かめてから使い、待ちが 100 ms のときは
出来上がりの md5 が `disp.ko.md5` と一致することも確かめる。本家の
`mkapp/app/ko/disp.ko` は Goggle 2 の 9.5.1 の /mnt/app/ko/disp.ko と同じもの
(md5 01f5f9a0、2026-10-30 に確認)。

純正 9.5.1 の /mnt/app/ko/disp.ko (md5 01f5f9a0adf42fee6b159f35113cff89、
logs/goggle2/2026-10-15-flash-backup/mnt_app.tar の app/ko/disp.ko) の写しで、
4 バイトだけ違う。

- 場所: .text+0x12e54 (ファイルの 0x12eac)、disp_vdpo_disable() の最後。
- 元: `4f f4 7a 70` = `mov.w r0, #1000` → `bl disp_delay_ms` (.rel.text で確認)。
- 後: `4f f0 64 00` = `mov.w r0, #100`。
- md5: 36f9ccd6d978ce303a599a348bdb64f9。

最初の版 (2026-10-16、md5 622b7255...) は disp_tv_disable() の同じ待ちを
書き換えていたが、VDPO の切り替えは disp_vdpo_disable() を通るので効かなかった
(dmesg の [VDPO] disable → enable が 1.095 秒のまま)。このモジュールには
disp_delay_ms(1000) を呼ぶ所が disp_tv_disable、disp_vdpo_disable、
disp_capture_dump_store、disp_device_attached_and_enable (TV は 300、HDMI は
1000) にあり、VDPO (出力 16) の切り替えで通るのは disp_vdpo_disable だけ。

この 1000 ms の待ちは、画面の出力先やモードを切り替えるたび
(`dispw -s vdpo ...`、起動時の 1080p50 と、入力の切り替えごと) に入っていた。
dmesg の `[VDPO]` の disable から enable までが 1.08〜1.10 秒だったのがそれ。
公開ソース (lindenis-v536-lichee-linux-4.9 の disp2/disp/de/disp_tv.c) の
disp_tv_disable() と同じ形の待ちが、このモジュール独自の disp_vdpo_disable() にもある。

確かめ方: 起動後の dmesg で `[VDPO]` の disable → enable の間隔が 0.1 秒ほどに
なっていること、入力を切り替えても画面が乱れないこと。乱れるなら
`4f f0 64 00` の `64` (100 ms) を大きくして作り直す (`make-disp-ko.sh` の
3 つ目の引数、1-255 ms)。

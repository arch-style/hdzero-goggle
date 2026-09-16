#!/bin/sh
#
# テスト用 SD カードを最新のビルドに更新する (Mac 側で実行)。
#
#   utilities/sdcard-test/update-sd.sh [/Volumes/<SDカード>]
#
# やること:
#   1. 9.5.0 ベースの fork バイナリをカードのルートへコピー
#   2. 空の RECORD.log を用意する。これがあると rc.sh が record プロセス自身の
#      出力をそこへ流す。DVR の失敗理由 (avio_open の EROFS など) は
#      アプリのログではなく record 側にしか出ない。
#      fork が起動ごとに boot-logs/RECORD.NNNN.log へ退避して作り直すので、
#      置くのは最初の一度だけでよい。
#   3. check-sd.sh を通す
#
# develop.sh は触らない。Stage の切り替えは手で行う。

SD=${1:-/Volumes/HDZERO}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILT="$ROOT/../hdzero-goggle-9.5/out/HDZGOGGLE"

if [ ! -d "$SD" ]; then
	echo "NG  $SD が無い。カードを挿してから実行する"
	exit 1
fi
if [ ! -f "$BUILT" ]; then
	echo "NG  $BUILT が無い。先にビルドする (README.md 手順 3)"
	exit 1
fi
if [ ! -f "$SD/develop.sh" ]; then
	echo "NG  $SD/develop.sh が無い。先に develop.sh.stage1 を置く"
	exit 1
fi

cp "$BUILT" "$SD/HDZGOGGLE" || exit 1
echo "ok  HDZGOGGLE を更新"

if [ -f "$SD/RECORD.log" ]; then
	echo "ok  RECORD.log は既にある"
else
	: > "$SD/RECORD.log" || exit 1
	echo "ok  空の RECORD.log を作成"
fi

sync
echo
exec "$ROOT/utilities/sdcard-test/check-sd.sh" "$SD"

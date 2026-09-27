#!/bin/sh
# ゴーグルに WiFi 経由 (SSH) で入り、ビルドの転送・再起動・ログ取得を行う。
# SD カードは刺したままでよい。カードの抜き差しも、アンマウントも要らない。
#
# 前提: ゴーグルの WiFi ページで WiFi を STA モード (自宅の Wi-Fi に参加) で
# 有効化し、SSH をオンにしていること。AP モードだとゴーグルが自分のネットワーク
# (192.168.2.x) を作るので、Mac 側をそちらに繋ぎ替えない限り届かない。
#
# パスワードは環境変数 GOGGLE_PW か ~/.config/hdzero-goggle/root_pw から読む。
# どちらも無ければ純正の既定値 "divimath"。expect で流し込むので sshpass は
# 要らない。
#
# 使い方:
#   goggle.sh <ip> push [file]   ビルドを /mnt/extsd/HDZGOGGLE に置いて sha1 を照合
#   goggle.sh <ip> restart       アプリだけ再起動 (develop.sh をやり直す)
#   goggle.sh <ip> reboot        ゴーグルごと再起動
#   goggle.sh <ip> log           HDZGOGGLE.log を取ってきて標準出力へ
#   goggle.sh <ip> tail          ログを追いかける (Ctrl-C で抜ける)
#   goggle.sh <ip> run <cmd...>  任意のコマンド
#
# Mac の scp は既定で SFTP を使うが dropbear に SFTP サーバは無いので -O で
# 旧プロトコルを使う。ホスト鍵は起動のたびに作り直されることがあるので照合しない。

set -e

IP=$1
CMD=$2
[ -n "$IP" ] && [ -n "$CMD" ] || { sed -n '2,25p' "$0"; exit 2; }
shift 2

PW=${GOGGLE_PW:-}
[ -z "$PW" ] && [ -r "$HOME/.config/hdzero-goggle/root_pw" ] && PW=$(cat "$HOME/.config/hdzero-goggle/root_pw")
[ -z "$PW" ] && PW=divimath

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
HERE=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$HERE/build_goggle2/out/HDZGOGGLE

# expect で ssh/scp のパスワード入力を済ませる。引数はそのまま spawn へ。
with_pw() {
	PW="$PW" expect -c '
		set timeout -1
		spawn {*}$argv
		expect {
			-re "(?i)password:" { send "$env(PW)\r"; exp_continue }
			eof
		}
		lassign [wait] pid spawnid os_error status
		exit $status
	' -- "$@"
}

gssh() { with_pw ssh $SSH_OPTS "root@$IP" "$@"; }

case "$CMD" in
push)
	FILE=${1:-$BUILD}
	[ -f "$FILE" ] || { echo "no such file: $FILE" >&2; exit 1; }
	LOCAL=$(shasum "$FILE" | cut -c1-40)
	with_pw scp -O $SSH_OPTS "$FILE" "root@$IP:/mnt/extsd/HDZGOGGLE.new"
	REMOTE=$(gssh "mv /mnt/extsd/HDZGOGGLE.new /mnt/extsd/HDZGOGGLE && chmod +x /mnt/extsd/HDZGOGGLE && sync && sha1sum /mnt/extsd/HDZGOGGLE" | grep -o '^[0-9a-f]\{40\}' | tail -1)
	echo "local  $LOCAL"
	echo "goggle $REMOTE"
	[ "$LOCAL" = "$REMOTE" ] || { echo "sha1 mismatch" >&2; exit 1; }
	;;
restart)
	# develop.sh は最後にアプリを & で起動して抜ける。ssh セッションが閉じても
	# 道連れにならないよう setsid で切り離す。
	gssh "killall HDZGOGGLE 2>/dev/null; sleep 1; (setsid sh /mnt/extsd/develop.sh </dev/null >/dev/null 2>&1 &); sleep 2; pidof HDZGOGGLE || echo 'not running'"
	;;
reboot)
	gssh "sync; reboot" || true
	;;
log)
	gssh "cat /mnt/extsd/HDZGOGGLE.log"
	;;
tail)
	gssh "tail -n 30 -f /mnt/extsd/HDZGOGGLE.log"
	;;
run)
	gssh "$@"
	;;
*)
	echo "unknown command: $CMD" >&2
	exit 2
	;;
esac

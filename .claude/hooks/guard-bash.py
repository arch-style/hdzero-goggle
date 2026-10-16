#!/usr/bin/env python3
"""PreToolUse (Bash) guard for the goggle 2 work. See CLAUDE.md.

deny: soft reboot of the goggle, running the app binary over SSH, LD_TRACE probes.
ask:  update files onto an SD card, flash writes on the goggle, Mac network changes.
Everything else passes untouched (no output, exit 0).
"""
import json
import re
import sys

try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
if data.get("tool_name") != "Bash":
    sys.exit(0)
cmd = (data.get("tool_input") or {}).get("command") or ""

# Start of a shell command: line start, ; & | ( or a quote that opens an ssh argument.
SEP = r"(?:^|[;&|(\n'\"]|\bthen\b|\bdo\b|\belse\b)\s*"
REMOTE = re.compile(r"\b(?:ssh|g2ssh(?:\.exp)?|expect|dropbear)\b|root@")

FW = (r"HDZG_OS\.bin|HDZG_BOOT\.bin|HDZGOGGLE_RX\.bin|HDZGOGGLE_VA\.bin|"
      r"HDZBOXPRO_(?:RX|VA)\.bin|HDZB_(?:OS|BOOT)\.bin|HDZERO_TX\.bin|"
      r"HDZERO_(?:GOGGLE2?|BOXPRO)[-_][^\s'\"]*\.bin|hdzgoggle-services-")
FLASH_TOOLS = (r"\b(?:mtd_debug|flash_erase|flashcp|nandwrite|fw_setenv|ota-burn\w*|"
               r"write_flashes(?:\.sh)?|update_goggle(?:\.sh)?|goggle2-install-hooks(?:\.sh)?|"
               r"goggle2-restore-setting(?:\.sh)?)\b|of=/dev/mtd")
FLASH_DIRS = r"(?:/mnt/app|/overlay|/etc)/"
END = r"\S*\s*(?:$|[;&|\n'\"])"


def out(decision, reason):
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": decision,
        "permissionDecisionReason": reason}}, ensure_ascii=False))
    sys.exit(0)


remote = bool(REMOTE.search(cmd))

# 1. Soft reboot never comes back on goggle 2 (2026-10-16).
if remote and (re.search(SEP + r"(?:sudo\s+)?(?:/usr)?(?:/s?bin/)?(?:reboot|poweroff|halt|shutdown)\b", cmd)
               or re.search(SEP + r"init\s+[06]\b", cmd) or "sysrq-trigger" in cmd):
    out("deny", "ゴーグルの reboot/poweroff は禁止 (2026-10-16 に戻らなかった)。"
                "ユーザーに電源ボタンでの入れ直しを頼むこと。CLAUDE.md 参照。")

# 2. Never run the app binary to probe it (2026-10-16 a second instance started).
if remote and ("LD_TRACE_LOADED_OBJECTS" in cmd or re.search(
        SEP + r"(?:[A-Za-z_][A-Za-z0-9_]*=\S*\s+)*(?:nohup\s+|exec\s+)?(?:\S*/)?HDZGOGGLE(?:\.v1|\.staged)?(?=\s|$|[;&|)'\"])",
        cmd)):
    out("deny", "アプリ本体を SSH から直接実行しない (musl は LD_TRACE を無視し、2 つ目のアプリが動いた)。"
                "読み込みを調べるなら /lib/ld-musl-armhf.so.1 --list。CLAUDE.md 参照。")

# 3. Update files on a card are written to flash at the next boot.
if re.search(FW, cmd) and re.search(r"/Volumes/|/mnt/extsd", cmd) and re.search(
        r"\b(?:cp|mv|rsync|ditto|scp|install|ln|tee|dd)\b|>|\bof=", cmd):
    out("ask", "SD カードに更新ファイル (HDZG_OS.bin など) を書こうとしている。起動しただけでフラッシュに書かれる。"
               "復旧カードを作るときだけ、ユーザーの了承を得て進める。")

# 4. Flash writes on the goggle need the user's OK every time.
if remote and (re.search(FLASH_TOOLS, cmd)
               or re.search(r">>?\s*" + FLASH_DIRS, cmd)
               or re.search(r"\b(?:cp|mv|install|ln)\b[^;&|\n]*\s" + FLASH_DIRS + END, cmd)
               or re.search(r"\b(?:rm|sed\s+-i)\b[^;&|\n]*\s" + FLASH_DIRS, cmd)
               or re.search(r"@[\w.-]+:" + FLASH_DIRS + END, cmd)
               # The target hidden in a variable or reached with cd (2026-10-17:
               # D=/etc/init.d/rc.final; cp $N $D.new; mv $D.new $D passed).
               or (re.search(r"(?:\b[A-Za-z_][A-Za-z0-9_]*=|\bcd\s+)['\"]?" + FLASH_DIRS, cmd)
                   and re.search(r"\b(?:cp|mv|rm|tee|install|ln|chmod|chown|dd|sed\s+-i)\b|>", cmd))):
    out("ask", "ゴーグルのフラッシュ (/mnt/app, /overlay, /etc, mtd, env) に書く操作。"
               "前提と、失敗したときの戻し方を確かめ、ユーザーの了承を得てから。")

# 5. Network changes on the Mac (public Wi-Fi restraint).
if re.search(r"\bsudo\b[^;&|\n]*\b(?:ifconfig|route|networksetup|arp)\b", cmd) or re.search(
        r"\bnetworksetup\s+-set|\bifconfig\s+\S+\s+(?:alias|inet\s)", cmd):
    out("ask", "Mac のネットワーク設定を変える操作。公共の Wi-Fi では行わない約束。今いる場所を確かめてから。")

sys.exit(0)

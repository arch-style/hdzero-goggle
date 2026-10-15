#!/usr/bin/env python3
"""
boot-trace.sh が残した boot-trace/NNN/ を読んで、起動の内訳を出す。

    utilities/sdcard-test/boot-trace-report.py [boot-trace のパス | NNN のパス]

出すもの (すべてカーネル起動からの秒。ブートローダは含まない):
  - カーネルが init に制御を渡した時刻 (dmesg の "Freeing unused kernel memory"
    / "Run /init" / "Run /sbin/init")
  - dmesg の中で前の行から 0.3 秒以上空いた所 (遅いドライバ、待ち)
  - プロセスの開始時刻を古い順に (init、rcS、rc.sh、record、develop.sh ...)
  - develop.sh に入った時刻
これで「13 秒」がカーネル・init スクリプト・rc.sh のどこに落ちているかが分かる。
"""

import re
import sys
from pathlib import Path

DMESG_TS = re.compile(r'^\[\s*(\d+\.\d+)\]\s?(.*)')
HANDOFF = ('Freeing unused kernel memory', 'Run /init', 'Run /sbin/init', 'Run /linuxrc')


def read_uptime(d):
    v = {}
    for line in (d / 'uptime.txt').read_text(errors='replace').splitlines():
        parts = line.split()
        if len(parts) == 2:
            v[parts[0]] = parts[1]
    return v


def report_dmesg(d, gap=0.3):
    p = d / 'dmesg.txt'
    if not p.exists():
        print('dmesg.txt が無い')
        return
    lines = []
    for line in p.read_text(errors='replace').splitlines():
        m = DMESG_TS.match(line)
        if m:
            lines.append((float(m.group(1)), m.group(2)))
    if not lines:
        print('dmesg に時刻が無い (CONFIG_PRINTK_TIME が無効)。行数だけ:',
              sum(1 for _ in p.open(errors='replace')))
        return
    print(f'dmesg: {len(lines)} 行, 最初 {lines[0][0]:.3f}s, 最後 {lines[-1][0]:.3f}s')
    for t, msg in lines:
        if any(h in msg for h in HANDOFF):
            print(f'  カーネル → init: {t:.3f}s  ({msg[:70]})')
            break
    else:
        print('  カーネル → init の行が見つからない。最後の行の時刻を上限として見る')
    print(f'  {gap:.1f}s 以上空いた所:')
    prev = None
    for t, msg in lines:
        if prev is not None and t - prev[0] >= gap:
            print(f'    {prev[0]:8.3f}s  {prev[1][:60]}')
            print(f'    {t:8.3f}s  +{t - prev[0]:.3f}s  {msg[:60]}')
        prev = (t, msg)


def report_procs(d, tck):
    p = d / 'procs.txt'
    if not p.exists():
        print('procs.txt が無い')
        return
    print(f'プロセス開始時刻 (tick={tck}/s):')
    for line in p.read_text(errors='replace').splitlines():
        parts = line.split(None, 3)
        if len(parts) < 3 or not parts[0].isdigit():
            continue
        ticks, pid, ppid = int(parts[0]), parts[1], parts[2]
        cmd = parts[3] if len(parts) > 3 else ''
        if cmd.startswith('[') and ticks == 0:
            continue  # カーネルスレッド
        print(f'  {ticks / tck:8.2f}s  pid {pid:>5} ppid {ppid:>5}  {cmd[:70]}')


def main():
    arg = Path(sys.argv[1] if len(sys.argv) > 1 else '/Volumes/NO NAME/boot-trace')
    if (arg / 'uptime.txt').exists():
        runs = [arg]
    else:
        runs = sorted(p for p in arg.glob('[0-9][0-9][0-9]') if (p / 'uptime.txt').exists())
    if not runs:
        sys.exit(f'{arg} に boot-trace の結果が無い')

    for d in runs:
        up = read_uptime(d)
        tck_s = up.get('clk_tck', 'unknown')
        tck = int(tck_s) if tck_s.isdigit() else 100
        print(f'=== {d} ===')
        print(f'develop.sh 到達: {up.get("entry", "?")}s   採取 {up.get("trace_start", "?")}s'
              f' - {up.get("trace_end", "?")}s')
        init = (d / 'init-cmdline.txt')
        if init.exists():
            print(f'init: {init.read_text(errors="replace").strip()}')
        cmd = (d / 'cmdline.txt')
        if cmd.exists():
            print(f'cmdline: {cmd.read_text(errors="replace").strip()[:200]}')
        report_dmesg(d)
        report_procs(d, tck)
        who = d / 'etc' / 'who-calls-rc.txt'
        if who.exists() and who.read_text().strip():
            print('rc.sh を呼ぶもの:', who.read_text().split())
        print()


if __name__ == '__main__':
    main()

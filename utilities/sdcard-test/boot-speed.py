#!/usr/bin/env python3
"""
起動速度を純正と fork で比べる。

    utilities/sdcard-test/boot-speed.py [ab-logs のパス]

develop.sh.abtest が残す ab-logs/NNN-stock.log / NNN-fork.log を読み、
1 起動ごとに「電源投入から映像が出るまで」を出す。

測り方。ログの先頭の時刻は 1970 年から始まる。RTC に電池が無く、起動直後は
実時計が未設定で、その値がそのまま起動からの経過秒になっているため。途中で
rtc_init が時計を偽の 2025 年に飛ばすので、飛ぶ直前の 1970 の行と直後の
2025 の行を突き合わせて、以降の行も起動からの経過秒に戻している。

映像が出た時刻は "OLED: Display on" で取る。パネルに火が入る行で、純正にも
fork にも同じ場所にある (oled.c の OLED_display)。ただしこの行は起動中に
何度か出るので、映像への切り替えを始める "switch to bw:" より後の最初の 1 本
だけを見る。fork は自前で "boot: picture at Nms" も出すので、突き合わせて
ずれていないことを確かめられる。

純正の起動時刻は fork より遅いことがある。fork はカードから、純正は
フラッシュから読み込まれるので、実行ファイルの読み込みにかかる時間が違う。
それも差のうちなので、アプリが最初の行を書いた時刻も併せて出す。
"""

import re
import statistics
import sys
from pathlib import Path

TS = re.compile(r'^(\d{4})-\d\d-\d\d (\d\d):(\d\d):(\d\d)\.(\d{3}) ')


def seconds(h, m, s, ms):
    return h * 3600 + m * 60 + s + ms / 1000.0


def parse(path):
    """1 つのログから (アプリ開始, 映像, fork 自身の申告) を起動からの秒で返す。"""
    first_uptime = None   # 最初の行 = アプリが動き出した時刻
    last_1970 = None      # 時計が飛ぶ直前
    first_2025 = None     # 飛んだ直後
    switch_seen = False
    picture_wall = None
    fork_claim = None

    for line in path.read_text(errors='replace').splitlines():
        m = TS.match(line)
        if not m:
            continue
        year = int(m.group(1))
        t = seconds(int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))

        if year < 2000:
            if first_uptime is None:
                first_uptime = t
            last_1970 = t
            continue

        if first_2025 is None:
            first_2025 = t

        if 'switch to bw:' in line:
            switch_seen = True
        elif switch_seen and picture_wall is None and 'OLED: Display on' in line:
            picture_wall = t

        if fork_claim is None:
            c = re.search(r'boot: picture at (\d+)ms', line)
            if c:
                fork_claim = int(c.group(1)) / 1000.0

    if first_uptime is None or last_1970 is None or first_2025 is None:
        return None

    # 2025 の時刻を起動からの秒に戻す係数。
    def to_uptime(wall):
        return last_1970 + (wall - first_2025)

    return {
        'app_start': first_uptime,
        'picture': to_uptime(picture_wall) if picture_wall is not None else None,
        'fork_claim': fork_claim,
    }


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else '/Volumes/HDZERO/ab-logs')
    if not d.is_dir():
        sys.exit(f'{d} が無い。カードを挿すか、コピーした場所を指定する')

    logs = sorted(p for p in d.glob('*.log'))
    if not logs:
        sys.exit(f'{d} にログが無い')

    groups = {}
    print(f'{"ログ":<22} {"種別":<6} {"アプリ開始":>10} {"映像まで":>10} {"fork申告":>10}')
    print('-' * 62)
    for p in logs:
        which = p.stem.split('-', 1)[1] if '-' in p.stem else '?'
        r = parse(p)
        if r is None:
            print(f'{p.name:<22} {which:<6} {"読めない":>10}')
            continue
        pic = r['picture']
        claim = r['fork_claim']
        print(f'{p.name:<22} {which:<6} {r["app_start"]:>9.2f}s '
              f'{(f"{pic:.2f}s" if pic else "-"):>10} '
              f'{(f"{claim:.2f}s" if claim else "-"):>10}')
        if pic is not None:
            groups.setdefault(which, []).append(pic)

    print()
    if not groups:
        sys.exit('映像に到達した起動が無い')

    print(f'{"種別":<8} {"回数":>4} {"中央値":>9} {"最小":>9} {"最大":>9}')
    print('-' * 44)
    med = {}
    for which, vals in sorted(groups.items()):
        med[which] = statistics.median(vals)
        print(f'{which:<8} {len(vals):>4} {med[which]:>8.2f}s {min(vals):>8.2f}s {max(vals):>8.2f}s')

    if 'stock' in med and 'fork' in med:
        d_s = med['stock'] - med['fork']
        print()
        print(f'差 (中央値): {d_s:+.2f}s   fork は純正の {med["fork"] / med["stock"] * 100:.0f}%')
        if min(len(v) for k, v in groups.items() if k in ('stock', 'fork')) < 3:
            print('※ 各 3 回未満。中央値としては当てにならないので、もう何回か回す')


if __name__ == '__main__':
    main()

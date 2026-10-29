#!/usr/bin/env python3
"""fill-probe.sh の結果 (boot-logs/fill-stage<段階>.txt) を段階ごとに並べる。

    python3 -I utilities/sdcard-test/fill-report.py /Volumes/<カード>

同じ段階の early-stage<段階>.txt (S10udev の時点の dmesg) から、マウント時の
FAT の読み込み (fat len の行から Read one fat の行まで) と、ディレクトリの先読み
(Read fat dir の行まで) の時間も出す。
"""
import os
import re
import sys


def fat_times(path):
    """dmesg からマウント時の (fat len, FAT 読み ms, dir 読み ms)。"""
    t_len = t_fat = t_dir = None
    fat_len = None
    try:
        with open(path, 'rb') as f:
            for raw in f:
                line = raw.decode('latin-1')
                m = re.match(r'\[\s*([0-9.]+)\]', line)
                if not m:
                    continue
                t = float(m.group(1))
                m2 = re.search(r'mmcblk0p1\): fat start \d+,fat len (\d+)', line)
                if m2:
                    fat_len, t_len = int(m2.group(1)), t
                elif 'mmcblk0p1): Read one fat' in line and t_len is not None:
                    t_fat = t
                elif 'Read fat dir and no rele' in line and t_fat is not None:
                    t_dir = t
                    break
    except OSError:
        return None
    if t_fat is None:
        return None
    fat_ms = (t_fat - t_len) * 1000
    dir_ms = (t_dir - t_fat) * 1000 if t_dir is not None else None
    return fat_len, fat_ms, dir_ms


def main():
    sd = sys.argv[1] if len(sys.argv) > 1 else '/Volumes/HDZERO_BOOT'
    logs = os.path.join(sd, 'fork-deletable', 'boot-logs')
    stages = sorted(int(m.group(1)) for n in os.listdir(logs)
                    if (m := re.fullmatch(r'fill-stage(\d+)\.txt', n)))
    if not stages:
        print('fill-stage*.txt が無い (fill-probe.sh がまだ一度も走っていない)')
        return 1
    for s in stages:
        print(f'=== 段階 {s}')
        ft = fat_times(os.path.join(logs, f'early-stage{s}.txt'))
        if ft:
            fat_len, fat_ms, dir_ms = ft
            d = f'{dir_ms:.0f}' if dir_ms is not None else '?'
            print(f'  mount     fat len {fat_len}: FAT 読み {fat_ms:.0f} ms, dir 読み {d} ms')
        with open(os.path.join(logs, f'fill-stage{s}.txt'), 'rb') as f:
            for raw in f:
                line = raw.decode('latin-1').rstrip()
                if line.startswith('df: '):
                    print('  ' + line)
                    continue
                if line.startswith('fsck-tail: '):
                    print('  ' + line)
                    continue
                if line.startswith('--- ') or line.startswith('fill-probe end'):
                    print('  ' + line)
                    continue
                m = re.match(r'probe (\w+) (.*)', line)
                if not m:
                    continue
                kind, rest = m.groups()
                kv = dict(x.split('=', 1) for x in rest.split() if '=' in x)
                where = kv.get('dir', kv.get('dev', ''))
                where = where.replace('/mnt/extsd/', '').replace('/mnt/extsd', '(直下)')
                extra = ''
                if 'create_ms' in kv:
                    extra = f'  作成 {kv["create_ms"]} ms'
                if 'rc' in kv:
                    extra = f'  rc={kv["rc"]}'
                files = kv.get('files', '')
                files = f'{files:>5} 本' if files else '       '
                print(f'  {kind:<7} {where:<22} {files}  {kv.get("ms", "?"):>6} ms{extra}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

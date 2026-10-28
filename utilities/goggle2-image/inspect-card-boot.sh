#!/bin/sh
# カードの隠れた起動領域を読むだけで調べる (PhoenixCard のカードや自作のカードの確認用、2026-10-20)。
# 使い方: sudo sh inspect-card-boot.sh /dev/rdiskN
dev=$1; [ -n "$dev" ] || { echo "usage: $0 /dev/rdiskN"; exit 2; }
T=$(mktemp -d /tmp/cardinspect.XXXXXX)
dd if="$dev" bs=512 skip=16 count=1 of="$T/boot0.bin" 2>/dev/null
dd if="$dev" bs=512 skip=32800 count=1 of="$T/pkg.bin" 2>/dev/null
dd if="$dev" bs=512 skip=33098 count=1 of="$T/uboot_head.bin" 2>/dev/null
dd if="$dev" bs=512 skip=40960 count=32 of="$T/mbr.bin" 2>/dev/null
python3 -I - "$T" <<'PY'
import struct, sys, zlib, os
T = sys.argv[1]
b0 = open(f'{T}/boot0.bin','rb').read(); pk = open(f'{T}/pkg.bin','rb').read()
uh = open(f'{T}/uboot_head.bin','rb').read(); mbr = open(f'{T}/mbr.bin','rb').read()
print('sector 16   :', 'boot0 eGON.BT0' if b0[4:12]==b'eGON.BT0' else f'no boot0 magic ({b0[4:12]!r})')
print('sector 32800:', 'sunxi-package' if pk[:13]==b'sunxi-package' else f'no package magic ({pk[:13]!r})')
if pk[:13]==b'sunxi-package':
    n, total = struct.unpack_from('<II', pk, 0x20); print(f'   items {n} total {total} B')
    wm, st = struct.unpack_from('<ii', uh, 0xe4)
    print(f'   u-boot head at sector 33098: magic {uh[4:9]!r} work_mode {wm:#x} ({"card product" if wm==0x11 else "normal boot" if wm==0 else "other"}) storage_type {st}')
crc, ver, magic, copy, index, count = struct.unpack_from('<II8sIII', mbr, 0)
ok = zlib.crc32(mbr[4:16384]) & 0xffffffff == crc
print(f'sector 40960: sunxi MBR magic {magic!r} version {ver:#x} copies {copy} index {index} partitions {count} crc {"ok" if ok else "BAD"}')
if magic == b'softw411' and 0 < count <= 120:
    for i in range(count):
        base = 32 + i*128
        _, addr, _, ln = struct.unpack_from('<IIII', mbr, base)
        name = mbr[base+32:base+48].split(b'\0',1)[0].decode(errors='replace')
        print(f'   {name:10} logical start {addr:8} sectors, length {ln:8} sectors ({ln*512/1024/1024:.1f} MB)')
PY
rm -rf "$T"

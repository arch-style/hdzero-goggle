#!/usr/bin/env python3
# Goggle 2 をカードから起動するための像を作る (段階 2-b、docs/goggle2-card-boot.md)。
# 読むのは HDZG_OS.bin から展開した boot0_sdcard.fex と boot_package.fex、カーネルの uImage。
# 書くのは出力ディレクトリの中だけ (card-boot.img、env.bin、env.txt、layout.txt)。
# 像の先頭 = カードのセクタ 16。dd するときは seek=16 (セクタ 0 の DOS MBR には触らない)。
#
#   セクタ 16        boot0_sdcard.fex (eGON.BT0、check_sum を確かめる)
#   セクタ 32800     boot_package.fex (sunxi-package、add_sum と u-boot の work_mode=0 を確かめる)
#   セクタ 40960     論理 0。sunxi MBR を 4 部 (16 KB ずつ、crc32)
#   論理 128         区画 boot (6528 セクタ) = カーネルの uImage (ヘッダとデータの crc を確かめる)
#   論理 6656        区画 env (256 セクタ) = u-boot の環境 (crc32 + 128 KB)
#
# 使い方: python3 -I make-card-boot-image.py --items <展開ディレクトリ> --out <出力ディレクトリ>
#         [--kernel <uImage>] [--marker hdzcard=1]
import argparse, hashlib, os, struct, sys, zlib

STAMP = 0x5F0A6C39
SECTOR = 512
BOOT0_SECTOR = 16
PACKAGE_SECTOR = 32800
LOGICAL_SECTOR = 40960          # u-boot CONFIG_MMC_LOGICAL_OFFSET (20 MB)
MBR_COPIES = 4                  # SUNXI_MBR_COPY_NUM
MBR_SIZE = 16384
PART_BOOT_START = MBR_COPIES * MBR_SIZE // SECTOR   # 128
PART_BOOT_LEN = 6528            # sys_partition.fex の boot と同じ
PART_ENV_START = PART_BOOT_START + PART_BOOT_LEN     # 6656
PART_ENV_LEN = 256              # 128 KB
ENV_SIZE = 131072               # CONFIG_ENV_SIZE
UIMAGE_MAGIC = 0x27051956
PARTITIONS_NOR = 'boot@mtdblock1:rootfs@mtdblock2:overlay@mtdblock3:env@mtdblock4:bootlogo@mtdblock5:app@mtdblock6:UDISK@mtdblock7'

def addsum(buf, field_off):
    b = bytearray(buf)
    struct.pack_into('<I', b, field_off, STAMP)
    return sum(struct.unpack_from(f'<{len(b)//4}I', b, 0)) & 0xffffffff

def fail(msg):
    sys.exit(f'STOP: {msg}')

def check_boot0(d):
    if d[4:12] != b'eGON.BT0': fail('boot0 magic')
    chk, ln = struct.unpack_from('<II', d, 12)
    if ln != len(d): fail(f'boot0 length {ln} != file {len(d)}')
    if addsum(d, 12) != chk: fail('boot0 check_sum')
    dram = struct.unpack_from('<6I', d, 0x38)
    return ln, dram

def check_package(d):
    if d[:13] != b'sunxi-package': fail('package magic')
    magic, stored, serial, status, items, valid_len = struct.unpack_from('<IIIIII', d, 0x10)
    if magic != 0x89119800 or valid_len != len(d): fail('package head')
    if addsum(d[:valid_len], 0x14) != stored: fail('package add_sum')
    found = {}
    for i in range(items):
        base = 0x40 + i * 0x170
        name = d[base:base + 64].split(b'\0', 1)[0].decode()
        off, ln = struct.unpack_from('<II', d, base + 64)
        found[name] = (off, ln)
    if 'u-boot' not in found or 'dtb' not in found: fail('package items')
    off, ln = found['u-boot']
    ub = d[off:off + ln]
    if ub[4:9] != b'uboot': fail('u-boot magic')
    uchk, align, ulen = struct.unpack_from('<III', ub, 12)
    if ulen != ln or addsum(ub[:ulen], 12) != uchk: fail('u-boot check_sum')
    work_mode, storage_type = struct.unpack_from('<ii', ub, 0xe4)
    if work_mode != 0: fail(f'u-boot work_mode {work_mode:#x} is not 0 (normal boot)')
    return found, work_mode, storage_type

def check_uimage(d):
    magic, hcrc, _t, size, load, ep, dcrc = struct.unpack_from('>IIIIIII', d, 0)
    if magic != UIMAGE_MAGIC: fail('uImage magic')
    hdr = bytearray(d[:64]); struct.pack_into('>I', hdr, 4, 0)
    if zlib.crc32(bytes(hdr)) & 0xffffffff != hcrc: fail('uImage header crc')
    if 64 + size > len(d): fail('uImage truncated')
    if zlib.crc32(d[64:64 + size]) & 0xffffffff != dcrc: fail('uImage data crc')
    if 64 + size > PART_BOOT_LEN * SECTOR: fail('kernel does not fit the boot partition')
    name = d[32:64].split(b'\0', 1)[0].decode(errors='replace')
    return size, load, ep, name

def make_mbr(index, parts):
    b = bytearray(MBR_SIZE)
    struct.pack_into('<I8sIIII', b, 4, 0x200, b'softw411', MBR_COPIES, index, len(parts), 0)
    for i, (name, start, length, user_type) in enumerate(parts):
        base = 32 + i * 128
        struct.pack_into('<IIII', b, base, 0, start, 0, length)
        b[base + 16:base + 32] = b'DISK'.ljust(16, b'\0')
        b[base + 32:base + 48] = name.encode().ljust(16, b'\0')
        struct.pack_into('<III', b, base + 48, user_type, 0, 0)
    struct.pack_into('<I', b, 0, zlib.crc32(bytes(b[4:])) & 0xffffffff)
    return bytes(b)

def parse_env(d):
    data = d[4:]
    if zlib.crc32(data) & 0xffffffff != struct.unpack_from('<I', d, 0)[0]: fail('stock env crc')
    entries = data.split(b'\0')
    # 純正の像は crc の直後に 0x00 が 1 つあり (空の項目か旗の 1 バイト)、その後に項目が続く。
    # u-boot はこの配置で動いているので、同じ配置で作る (先頭の 0x00 の数を覚えておく)。
    lead = 0
    while lead < len(entries) and entries[lead] == b'':
        lead += 1
    pairs = []
    for s in entries[lead:]:
        if not s: break
        k, _, v = s.decode().partition('=')
        pairs.append((k, v))
    return pairs, lead

def make_env(pairs, lead):
    data = b'\0' * lead + b''.join(f'{k}={v}'.encode() + b'\0' for k, v in pairs) + b'\0'
    if len(data) > ENV_SIZE - 4: fail('env too big')
    data = data.ljust(ENV_SIZE - 4, b'\0')
    return struct.pack('<I', zlib.crc32(data) & 0xffffffff) + data

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--items', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--kernel')
    ap.add_argument('--marker', default='hdzcard=1')
    a = ap.parse_args()
    rd = lambda n: open(os.path.join(a.items, n), 'rb').read()
    boot0 = rd('boot0_sdcard.fex'); pkg = rd('boot_package.fex'); env0 = rd('env_nor.fex')
    kernel = open(a.kernel, 'rb').read() if a.kernel else rd('boot.fex')

    b0len, dram = check_boot0(boot0)
    items, wm, st = check_package(pkg)
    ksize, kload, kep, kname = check_uimage(kernel)

    pairs, lead = parse_env(env0)
    keys = [k for k, _ in pairs]
    for need in ('setargs_nor', 'boot_normal', 'nor_root', 'init', 'console', 'loglevel', 'cma', 'ion_carveout_list', 'earlyprintk', 'initcall_debug'):
        if need not in keys: fail(f'stock env lacks {need}')
    setargs_card = ('setenv bootargs earlyprintk=${earlyprintk} initcall_debug=${initcall_debug} console=${console} '
                    'loglevel=${loglevel} root=${nor_root} init=${init} partitions=' + PARTITIONS_NOR +
                    ' cma=${cma} ion_carveout_list=${ion_carveout_list} ' + a.marker)
    env = []
    for k, v in pairs:
        if k == 'bootcmd': v = 'run setargs_card boot_normal'
        if k == 'appOta': v = '0'
        if k == 'boot_normal' and 'bootm' not in v: fail(f'stock boot_normal is {v!r}, expected bootm')
        env.append((k, v))
    env.append(('setargs_card', setargs_card))
    bootcmd = dict(env)['bootcmd']
    if len(bootcmd) > 127: fail('bootcmd longer than 127 bytes')
    envbin = make_env(env, lead)

    parts = [('boot', PART_BOOT_START, PART_BOOT_LEN, 0x8000), ('env', PART_ENV_START, PART_ENV_LEN, 0x8000)]
    total_sectors = LOGICAL_SECTOR + PART_ENV_START + PART_ENV_LEN - BOOT0_SECTOR
    img = bytearray(total_sectors * SECTOR)
    def put(sector, data):
        off = (sector - BOOT0_SECTOR) * SECTOR
        if off < 0 or off + len(data) > len(img): fail(f'region at sector {sector} out of image')
        if any(img[off:off + len(data)]): fail(f'overlap at sector {sector}')
        img[off:off + len(data)] = data
    put(BOOT0_SECTOR, boot0)
    if BOOT0_SECTOR + (len(boot0) + SECTOR - 1) // SECTOR > PACKAGE_SECTOR: fail('boot0 overlaps package')
    put(PACKAGE_SECTOR, pkg)
    if PACKAGE_SECTOR + (len(pkg) + SECTOR - 1) // SECTOR > LOGICAL_SECTOR: fail('package overlaps the MBR area')
    for i in range(MBR_COPIES):
        put(LOGICAL_SECTOR + i * MBR_SIZE // SECTOR, make_mbr(i, parts))
    put(LOGICAL_SECTOR + PART_BOOT_START, kernel)
    put(LOGICAL_SECTOR + PART_ENV_START, envbin)

    os.makedirs(a.out, exist_ok=True)
    open(os.path.join(a.out, 'card-boot.img'), 'wb').write(img)
    open(os.path.join(a.out, 'env.bin'), 'wb').write(envbin)
    open(os.path.join(a.out, 'env.txt'), 'w').write(''.join(f'{k}={v}\n' for k, v in env))
    ksec = (len(kernel) + SECTOR - 1) // SECTOR
    regions = [
        ('boot0', BOOT0_SECTOR, (len(boot0) + SECTOR - 1) // SECTOR),
        ('boot_package', PACKAGE_SECTOR, (len(pkg) + SECTOR - 1) // SECTOR),
        ('sunxi_mbr_x4', LOGICAL_SECTOR, MBR_COPIES * MBR_SIZE // SECTOR),
        ('kernel', LOGICAL_SECTOR + PART_BOOT_START, ksec),
        ('env', LOGICAL_SECTOR + PART_ENV_START, PART_ENV_LEN),
    ]
    with open(os.path.join(a.out, 'layout.txt'), 'w') as f:
        f.write('# name start_sector sector_count md5 (of the card sectors; verify with verify-card-boot.sh)\n')
        for name, start, count in regions:
            off = (start - BOOT0_SECTOR) * SECTOR
            f.write(f'{name} {start} {count} {hashlib.md5(img[off:off + count * SECTOR]).hexdigest()}\n')
        f.write(f'# image: sectors {BOOT0_SECTOR}..{BOOT0_SECTOR + total_sectors} ({len(img)} B), dd seek={BOOT0_SECTOR}\n')
        f.write(f'# FAT partition must start at or after sector {BOOT0_SECTOR + total_sectors}\n')
    print(f'boot0: {len(boot0)} B, check_sum ok, dram_para[0..5] {[hex(x) for x in dram]}')
    print(f'package: {len(pkg)} B, add_sum ok, items {sorted(items)}, u-boot work_mode {wm} storage_type {st}, u-boot check_sum ok')
    print(f'kernel: "{kname}" data {ksize} B load {kload:#x} entry {kep:#x}, header+data crc ok, {ksec} sectors of {PART_BOOT_LEN}')
    print(f'env: {len(env)} variables, bootcmd={bootcmd!r} ({len(bootcmd)} chars)')
    print(f'image: {len(img)} B = sectors {BOOT0_SECTOR}..{BOOT0_SECTOR + total_sectors}; md5 {hashlib.md5(img).hexdigest()}')
    print(f'written: {a.out}/card-boot.img env.bin env.txt layout.txt')

main()

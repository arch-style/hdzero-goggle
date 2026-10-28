#!/usr/bin/env python3
# Allwinner IMAGEWTY (version 0x300, 暗号化なし) の一覧と展開。読むだけ。
# 使い方: python3 -I imagewty-unpack.py <image> [出力ディレクトリ]
# 構造 (HDZG_OS.bin を xxd で読んで確かめた、2026-10-20):
#   先頭 0x60: "IMAGEWTY", version 0x300, header_size, ram_base, version,
#   image_size, 0, item_header_size (0x400), pid, vid, hw, fw, 1, 0x400, 項目数, 0x400
#   項目の見出しは 0x400 から 0x400 ごと: filename_len, header_size, maintype[8],
#   subtype[16], unknown, filename[256], stored_length, pad, original_length, pad, offset
import struct, sys, os

def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'IMAGEWTY':
        sys.exit('not an IMAGEWTY image')
    ver, hdr_size, ram_base, version, image_size, _z, item_hdr = struct.unpack_from('<IIIIIII', data, 8)
    pid, vid, hw, fw, one, k1, nfiles, k2 = struct.unpack_from('<IIIIIIII', data, 0x24)
    print(f'version 0x{ver:x} header_size 0x{hdr_size:x} image_size {image_size} ({len(data)} in file) items {nfiles} pid 0x{pid:x} vid 0x{vid:x} hw 0x{hw:x} fw 0x{fw:x}')
    print(f'{"maintype":9} {"subtype":17} {"filename":20} {"offset":>10} {"stored":>10} {"original":>10}')
    for i in range(nfiles):
        base = item_hdr + i * item_hdr
        fn_len, h_size = struct.unpack_from('<II', data, base)
        maintype = data[base+8:base+16].rstrip(b'\0 ').decode(errors='replace')
        subtype = data[base+16:base+32].rstrip(b'\0 ').decode(errors='replace')
        filename = data[base+0x24:base+0x124].split(b'\0', 1)[0].decode(errors='replace')
        stored, _p1, original, _p2, offset = struct.unpack_from('<IIIII', data, base+0x124)
        print(f'{maintype:9} {subtype:17} {filename:20} {offset:>10} {stored:>10} {original:>10}')
        if out:
            os.makedirs(out, exist_ok=True)
            name = os.path.basename(filename) or f'item{i}'
            with open(os.path.join(out, name), 'wb') as g:
                g.write(data[offset:offset+original])
    if out:
        print(f'extracted to {out}')

main()

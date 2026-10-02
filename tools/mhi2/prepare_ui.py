#!/usr/bin/env python3
"""Build separate QNX UI media using the existing host graphics bridge.

Keep the booted firmware's own HMI, startup and data. The diagnostic shell and
EGL/GLES/encoder bridge are emulator compatibility resources, not flash images.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib

from qnx6_read import Qnx6
sys.path.insert(0, str(Path(__file__).parent / 'glforward'))
from ifs_resource import add_file


def entries(raw, base=0):
    assert raw[base:base+7] == b'imagefs'
    pos = base + struct.unpack_from('<I', raw, base+16)[0]
    while (size := struct.unpack_from('<H', raw, pos)[0]):
        assert size >= 24 and pos + size <= len(raw)
        mode = struct.unpack_from('<I', raw, pos+8)[0]
        if mode & 0xf000 == 0x8000:
            offset, length = struct.unpack_from('<II', raw, pos+24)
            name = raw[pos+32:pos+size].split(b'\0')[0].decode()
            yield name, base+offset, length
        pos += size


def pack_lzo(raw):
    lib = ctypes.CDLL(ctypes.util.find_library('lzo2'))
    work = ctypes.create_string_buffer(16*1024*1024)
    block = 2*1024*1024
    out = bytearray(0x800)
    out[:8] = b'LZOZ' + struct.pack('<I', block)
    for index, offset in enumerate(range(0, len(raw), block)):
        data = bytes(raw[offset:offset+block])
        buf = ctypes.create_string_buffer(len(data)+len(data)//16+128)
        size = ctypes.c_size_t(len(buf))
        assert lib.lzo1z_999_compress(data, len(data), buf, ctypes.byref(size), work) == 0
        verify = ctypes.create_string_buffer(block)
        length = ctypes.c_size_t(block)
        assert lib.lzo1z_decompress_safe(buf, size, verify, ctypes.byref(length), None) == 0
        assert verify.raw[:length.value] == data
        struct.pack_into('<I', out, 8+4*index, size.value)
        out.extend(buf.raw[:size.value])
        out.extend(bytes(-len(out) % 512))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--main-ifs', type=Path, required=True)
    ap.add_argument('--app', type=Path, required=True)
    ap.add_argument('--formatted-disk', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    for path in (args.base / 'nor.bin', args.base / 'iram.bin',
                 args.main_ifs / 'imagefs.bin', args.app, args.formatted_disk):
        if not path.is_file():
            ap.error('Missing input: ' + str(path))
    # app is MMX2/app/70/default/app.img.
    metadata = args.app.parents[4] / 'metainfo2.txt'
    import re
    train = re.search(r'^release\s*=\s*"([^"]+)"', metadata.read_text(), re.M).group(1)
    args.output.mkdir(parents=True, exist_ok=False)
    nor = bytearray((args.base / 'nor.bin').read_bytes())
    kernel = nor[0x760000:0xa60000]
    raw = bytearray(zlib.decompress(kernel[0x800:]))
    base = 8 + struct.unpack_from('<I', raw, 40)[0]
    for name, offset, size in entries(raw, base):
        if name == 'usr/sbin/startup.sh':
            lines = [line.strip() for line in raw[offset:offset+size].splitlines()
                     if line.strip() and not line.strip().startswith(b'#')
                     and line.strip() != b'tinit &']
            script = (b'#!/bin/ksh\n(\n' + b'\n'.join(lines) +
                      b'\n)&\necho MHI2_DIAGNOSTIC_SHELL\nexec ksh -i\n')
            assert len(script) <= size
            raw[offset:offset+size] = script.ljust(size, b' ')
            break
    else:
        raise ValueError('No production startup script')
    packed = zlib.compress(raw, 9)
    header = kernel[:0x800]
    header[:8] = b'ANDROID!'
    struct.pack_into('<I', header, 8, len(packed))
    struct.pack_into('<I', header, 0x264, zlib.crc32(packed))
    struct.pack_into('<I', header, 0x7fc, zlib.crc32(header[:0x7fc]))
    assert len(header)+len(packed) <= 0x300000
    nor[0x760000:0xa60000] = (header+packed).ljust(0x300000, b'\xff')

    main = bytearray((args.main_ifs / 'imagefs.bin').read_bytes())
    replacements = {}
    for name, offset, size in entries(main):
        if name in ('lib/libEGL.so', 'lib/libGLESv2.so', 'lib/libnvvsenc.so'):
            data = Path('/tmp/mhi2-'+name[4:]).read_bytes()
            assert len(data) <= size
            main[offset:offset+size] = data.ljust(size, b'\0')
            replacements[name] = hashlib.sha256(data).hexdigest()
    assert len(replacements) == 3
    main = add_file(main, 'sbin/mhi2-pcm-endpoint', Path('/tmp/mhi2-pcm-endpoint').read_bytes())
    compressed = pack_lzo(main)
    assert len(compressed) <= 0x2a00000
    nor[0xa60000:0x3460000] = compressed.ljust(0x2a00000, b'\xff')
    (args.output / 'nor.bin').write_bytes(nor)
    (args.output / 'iram.bin').write_bytes((args.base / 'iram.bin').read_bytes())
    disk = args.output / 'emmc.raw'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always',
                    str(args.formatted_disk), str(disk)], check=True)
    with disk.open('r+b') as out:
        mbr = out.read(512)
        start, sectors = struct.unpack_from('<II', mbr, 454)
        assert mbr[450] == 177 and args.app.stat().st_size <= sectors*512
        out.seek(458)
        out.write(struct.pack('<I', args.app.stat().st_size//512))
        out.seek(start*512)
        with args.app.open('rb') as src:
            while chunk := src.read(8*1024*1024):
                out.write(chunk)
    fs = Qnx6(str(disk), start*512)
    ino = 1
    for name in ('img_restore', 'main_stage2.ifs.lzo'):
        ino = dict(fs.listdir(ino))[name]
    node = fs.inode(ino)
    assert len(compressed) <= node['size']
    payload = compressed.ljust(node['size'], b'\0')
    with disk.open('r+b') as out:
        for index, offset in enumerate(range(0, len(payload), fs.bs)):
            block = fs._bmap(node, index)
            assert block is not None
            out.seek(fs.base+(block+fs.sblks_off)*fs.bs)
            out.write(payload[offset:offset+fs.bs])
    subprocess.run([sys.executable, str(Path(__file__).with_name('extract_kanzi_shaders.py')),
                    '--app', str(args.app), '--output', str(args.output / 'shaders')], check=True)
    report = {'firmware_train': train, 'shader_cache': 'shaders', 'base': str(args.base), 'app': str(args.app),
              'graphics_bridge': replacements,
              'startup': 'production startup plus diagnostic shell',
              'hmi_executable': 'unchanged', 'emulator_only': True}
    (args.output / 'ui-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(args.output)


if __name__ == '__main__':
    main()

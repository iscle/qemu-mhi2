#!/usr/bin/env python3
"""Create a sparse emulator disk with K3342 app and blank data partitions.

The data partitions must be formatted with the guest's native mkqnx6fs before
normal boot. Partition types and ordering match the K3342 /etc/fstab.
"""
import argparse
import json
import shutil
import struct
from pathlib import Path


def entry(kind, start, count):
    return struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', kind,
                       b'\xfe\xff\xff', start, count)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('app', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    assert args.app.stat().st_size % 512 == 0
    app_sectors = args.app.stat().st_size // 512
    align = lambda n: (n + 2047) // 2048 * 2048
    board = align(2048 + app_sectors)
    ota = board + 64 * 2048
    ext = ota + 128 * 2048
    data = [('speech', 64), ('gracenotedb', 64), ('mmebackup', 64),
            ('icab', 64), ('adb', 64), ('gecache', 64), ('ols', 64),
            ('navdb', 256), ('media', 256)]
    layout = [('app', 2048, app_sectors), ('boardbook', board, 64 * 2048),
              ('ota', ota, 128 * 2048)]
    with args.output.open('xb') as dst:
        dst.truncate(4 * 1024**3)
        mbr = bytearray(512)
        for i, ent in enumerate([entry(177, 2048, app_sectors),
                                 entry(178, board, 64 * 2048),
                                 entry(179, ota, 128 * 2048),
                                 entry(5, ext, 4 * 1024**3 // 512 - ext)]):
            mbr[446 + 16*i:462 + 16*i] = ent
        mbr[510:] = b'\x55\xaa'
        dst.write(mbr)
        current = ext
        for i, (name, mib) in enumerate(data):
            count = mib * 2048
            ebr = bytearray(512)
            ebr[446:462] = entry(178, 2048, count)
            following = current + 2048 + count
            if i + 1 < len(data):
                ebr[462:478] = entry(5, following - ext,
                                     4 * 1024**3 // 512 - following)
            ebr[510:] = b'\x55\xaa'
            dst.seek(current * 512)
            dst.write(ebr)
            layout.append((name, current + 2048, count))
            current = following
        dst.seek(2048 * 512)
        with args.app.open('rb') as src:
            shutil.copyfileobj(src, dst, 8 * 1024**2)
    args.output.with_suffix('.layout.json').write_text(json.dumps(layout, indent=2)+'\n')
    print(args.output)


if __name__ == '__main__':
    main()

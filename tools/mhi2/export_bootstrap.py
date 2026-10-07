#!/usr/bin/env python3
"""Export the captured emulator bootstrap and native formatted data template.

The app partition is removed; prepare.py installs it from the user's archive.
Source files are read only. This is an emulator seed, not a flashable image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--nor', type=Path, required=True)
    ap.add_argument('--iram', type=Path, required=True)
    ap.add_argument('--formatted-disk', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(args.nor, args.output/'nor.bin')
    shutil.copyfile(args.iram, args.output/'iram.bin')
    disk = args.output/'emmc-template.raw'
    with args.formatted_disk.open('rb') as src, disk.open('xb') as dst:
        mbr = src.read(512)
        assert mbr[510:] == b'\x55\xaa' and mbr[450] == 177
        start, sectors = struct.unpack_from('<II', mbr, 454)
        src.seek(0)
        offset = 0
        while chunk := src.read(1024*1024):
            begin = max(offset, start*512)
            end = min(offset+len(chunk), (start+sectors)*512)
            if begin < end:
                chunk = bytearray(chunk)
                chunk[begin-offset:end-offset] = bytes(end-begin)
            if any(chunk):
                dst.seek(offset)
                dst.write(chunk)
            offset += len(chunk)
        dst.truncate(offset)
    manifest = {'version': 1, 'source': 'Previously tested variant-70 emulator capture',
                'emulator_only': True, 'app_partition': 'zero-filled; supply update archive',
                'sources': {p.name: digest(p) for p in (args.nor, args.iram, args.formatted_disk)},
                'files': {p.name: digest(p) for p in (args.output/'nor.bin', args.output/'iram.bin', disk)}}
    (args.output/'bootstrap.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(args.output)


if __name__ == '__main__':
    main()

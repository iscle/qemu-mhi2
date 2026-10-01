#!/usr/bin/env python3
"""Stage an extracted MIB2 map package using its own installation manifest.

This prepares emulator media offline; it does not run a head-unit update or
alter signatures/licensing. Shared MIB1 files referenced by content.pkg must
also have been extracted. The input firmware disk is preserved.
"""
import argparse
import configparser
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

import qnx6_image


def stage(package, output):
    manifest = configparser.ConfigParser(interpolation=None, strict=False)
    manifest.read(package / 'Mib2/metainfo2.txt')
    output.mkdir(parents=True, exist_ok=False)
    files = {}

    def install(source, dest):
        source = Path(os.path.abspath(source)).resolve(strict=True)
        source.relative_to(package.resolve())
        dest.relative_to(output)
        if dest in files:
            if source == files[dest]:
                return
            raise ValueError(f'Conflicting sources for {dest}')
        dest.parent.mkdir(parents=True, exist_ok=True)
        os.link(source, dest)
        files[dest] = source

    for section in manifest.sections():
        fields = manifest[section]
        target = fields.get('destination', '').strip('"')
        prefix = 'net/mmx/mnt/navdb/'
        if not target.lstrip('/').startswith(prefix):
            continue
        relative = Path(target.lstrip('/')[len(prefix):])
        if '..' in relative.parts:
            raise ValueError(target)
        dest = output / relative
        parts = section.split('\\')
        source = package / 'Mib2' / Path(*parts[:-1])
        source /= fields.get('source', '').strip('"')
        source = Path(os.path.abspath(source)).resolve(strict=True)
        if parts[-1] == 'File':
            install(source, dest)
        elif parts[-1] == 'Dir':
            for path in sorted(source.rglob('*')):
                if path.is_file():
                    install(path, dest / path.relative_to(source))
            # MIB2 reuses much of MIB1. These entries are part of the signed
            # content manifest, not optional files or symlinks in the archive.
            content = source / 'content.pkg'
            if content.exists() and section.startswith("NavDB\\"):
                for item in json.loads(content.read_text())['file']:
                    path = source / item.get('source', '') / item['name']
                    if path.stat().st_size != int(item['filesize']):
                        raise ValueError(f'Wrong size: {path}')
                    install(path, dest / item['name'])
    print(f'Staged {len(files)} files, {sum(p.stat().st_size for p in files)} bytes', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('package', type=Path)
    ap.add_argument('disk', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    staging = args.output.with_suffix('.navdb')
    stage(args.package, staging)
    image = args.output.with_suffix('.navdb.qnx6')
    qnx6_image.build(str(staging), str(image), slack_blocks=16384)
    install_disk(args.disk, args.output, image, args.package)


def install_disk(input_disk, output_disk, image, package):
    # Grow navdb in place and move the following media partition. Every EBR
    # link's enclosing size must also grow: QNX checks these nested bounds.
    with input_disk.open('rb') as disk:
        mbr = bytearray(disk.read(512))
        extended = struct.unpack_from('<I', mbr, 446 + 3*16 + 8)[0]
        ebr_sector = extended
        entries = []
        for index in range(9):
            disk.seek(ebr_sector*512)
            ebr = bytearray(disk.read(512))
            if ebr[510:] != b'\x55\xaa':
                raise ValueError('Invalid extended partition table')
            entries.append([ebr_sector, ebr])
            ebr_sector = extended + struct.unpack_from('<I', ebr, 462+8)[0]
        media_relative, media_count = struct.unpack_from('<II', entries[8][1], 454)
        disk.seek((entries[8][0]+media_relative)*512)
        media_data = disk.read(media_count*512)
    start = entries[7][0] + struct.unpack_from('<I', entries[7][1], 454)[0]
    count = (image.stat().st_size+511)//512
    entries[8][0] = (start+count+2047)//2048*2048
    end = entries[8][0]+media_relative+media_count
    disk_size = 1 << (max(input_disk.stat().st_size, end*512)-1).bit_length()
    struct.pack_into('<I', entries[7][1], 458, count)
    struct.pack_into('<I', mbr, 506, disk_size//512-extended)
    for index, (sector, ebr) in enumerate(entries[:-1]):
        following = entries[index+1][0]
        struct.pack_into('<II', ebr, 470, following-extended, disk_size//512-following)
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(input_disk), str(output_disk)], check=True)
    with output_disk.open('r+b') as disk, image.open('rb') as source:
        disk.truncate(disk_size)
        disk.seek(0); disk.write(mbr)
        for sector, ebr in entries:
            disk.seek(sector*512); disk.write(ebr)
        disk.seek((entries[8][0]+media_relative)*512); disk.write(media_data)
        disk.seek(start*512)
        shutil.copyfileobj(source, disk, 8*1024*1024)
    output_disk.with_suffix('.maps.json').write_text(json.dumps({
        'package': str(package), 'source_disk': str(input_disk),
        'navdb_start': start, 'navdb_sectors': count, 'disk_bytes': disk_size,
    }, indent=2)+'\n')
    print(output_disk, flush=True)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Install the firmware's speech resources on a separate experimental disk.

Rebuild the extended partition layout to accommodate the speech database while
preserving all other filesystems, including the navigation database.
"""
import argparse
import configparser
import fcntl
import sys
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

from sparse_copy import copy_sparse
import qnx6_image


def stage(package, output):
    manifest = configparser.ConfigParser(interpolation=None, strict=False)
    manifest.read(package / 'metainfo2.txt')
    output.mkdir(parents=True, exist_ok=False)
    count = size = 0
    for section in manifest.sections():
        fields = manifest[section]
        prefix = '/net/mmx/mnt/speech/'
        destination = fields.get('destination', '').strip('"')
        if not destination.startswith(prefix):
            continue
        parts = section.split('\\')
        if parts[-1] != 'Dir':
            raise ValueError(section)
        source = package.joinpath(*parts[:-1]) / fields.get('source', '').strip('"')
        relative = Path(destination[len(prefix):])
        if '..' in relative.parts:
            raise ValueError(destination)
        total = 0
        for item in sorted(source.rglob('*')):
            if not item.is_file():
                continue
            item.resolve().relative_to(package.resolve())
            target = output / relative / item.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            os.link(item, target)
            total += item.stat().st_size
            count += 1
        if total != int(fields['filesize'].strip('"')):
            raise ValueError(f'{section}: expected {fields["filesize"]}, got {total}')
        size += total
    if not count:
        raise ValueError('No speech resources found')
    return {'files': count, 'bytes': size}


def copy_range(source, target, src_offset, dst_offset, size):
    # Reflink when supported. Both disks are independent; overlapping positions
    # in the output therefore cannot overwrite a later input filesystem.
    try:
        if sys.platform != "linux":
            raise OSError("Reflink range is Linux-only")
        fcntl.ioctl(target.fileno(), 0x4020940D,
                    struct.pack('qQQQ', source.fileno(), src_offset, size, dst_offset))
        return
    except OSError:
        pass
    source.seek(src_offset)
    target.seek(dst_offset)
    while size:
        data = source.read(min(size, 8*1024*1024))
        if not data:
            raise EOFError('Truncated input filesystem')
        target.write(data)
        size -= len(data)


def install(input_disk, output_disk, image):
    if output_disk.exists():
        raise FileExistsError(output_disk)
    with input_disk.open('rb') as source:
        mbr = bytearray(source.read(512))
        extended = struct.unpack_from('<I', mbr, 502)[0]
        entries = []
        sector = extended
        seen = set()
        while sector not in seen:
            seen.add(sector)
            source.seek(sector*512)
            ebr = bytearray(source.read(512))
            if ebr[510:] != b'\x55\xaa':
                raise ValueError('Invalid EBR')
            relative, count = struct.unpack_from('<II', ebr, 454)
            entries.append((sector, relative, count, ebr))
            following = struct.unpack_from('<I', ebr, 470)[0]
            if not following:
                break
            sector = extended + following
        else:
            raise ValueError('Cyclic EBR chain')
        # K3342 layout: speech, gracenotedb, mmebackup, icab, adb, gecache,
        # ols, navdb, media. Check the input rather than changing an arbitrary disk.
        if len(entries) != 9 or entries[0][0]+entries[0][1] != 2387968:
            raise ValueError('Unexpected K3342 disk layout')
        starts = []
        sector = extended
        for index, (_, relative, count, _) in enumerate(entries):
            if index == 0:
                count = (image.stat().st_size+511)//512
            starts.append((sector, relative, count))
            sector = (sector+relative+count+2047)//2048*2048
        disk_size = max(input_disk.stat().st_size, sector*512)
        copy_sparse(input_disk, output_disk)
        with output_disk.open('r+b') as target:
            target.truncate(disk_size)
            for index, ((old, _, old_count, ebr), (new, relative, count)) in enumerate(zip(entries, starts)):
                if index == 0:
                    with image.open('rb') as resource:
                        copy_range(resource, target, 0, (new+relative)*512, image.stat().st_size)
                else:
                    copy_range(source, target, (old+relative)*512, (new+relative)*512, old_count*512)
                struct.pack_into('<II', ebr, 454, relative, count)
                if index+1 < len(starts):
                    following = starts[index+1][0]
                    struct.pack_into('<II', ebr, 470, following-extended, disk_size//512-following)
                target.seek(new*512)
                target.write(ebr)
            struct.pack_into('<I', mbr, 506, disk_size//512-extended)
            target.seek(0)
            target.write(mbr)
    return [{'ebr': s, 'start': s+r, 'sectors': n} for s, r, n in starts]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('disk', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    staging = args.output.with_suffix('.speech')
    metadata = stage(args.package, staging)
    image = args.output.with_suffix('.speech.qnx6')
    # Speech writes grammars, preferences and mount links at runtime. Reserving
    # data blocks alone leaves no free inodes on an otherwise empty new volume.
    qnx6_image.INODE_SLACK = 4096
    qnx6_image.build(str(staging), str(image), slack_blocks=16384)
    metadata['partitions'] = install(args.disk, args.output, image)
    metadata['source_disk'] = str(args.disk)
    metadata['package'] = str(args.package)
    args.output.with_suffix('.speech.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()

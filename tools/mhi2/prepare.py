#!/usr/bin/env python3
"""Assemble MHI2 variant 70 emulator media from an update and captured NOR/BIT.

This models the installer and BootROM handoff; it is NOT a flashable update.
No firmware instructions are patched and Quickboot still checks stage2 CMAC.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from sparse_copy import copy_sparse
from cryptography.hazmat.primitives.cmac import CMAC
from cryptography.hazmat.primitives.ciphers import algorithms


def cmac(data):
    c = CMAC(algorithms.AES(bytes(16)))
    c.update(bytes(data))
    return c.finalize()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--extracted', type=Path, required=True, help='directory containing MMX2')
    ap.add_argument('--base-nor', type=Path, required=True)
    ap.add_argument('--base-iram', type=Path, required=True)
    ap.add_argument('--base-emmc', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True, help='new output directory')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    nor = bytearray(args.base_nor.read_bytes())
    iram = bytearray(args.base_iram.read_bytes())
    assert len(nor) == 64 * 1024**2
    bct = struct.unpack_from('<I', iram, 0x3c)[0] - 0x40000000
    assert bct >= 0 and bct + 0x17f0 <= len(iram)
    parts = {}
    assert struct.unpack_from('<I', nor, 0x40040)[0] == 13
    for i in range(13):
        o = 0x40048 + i * 120
        pid = struct.unpack_from('<I', nor, o)[0]
        name = bytes(nor[o+4:o+24]).split(b'\0')[0].decode()
        start, size = struct.unpack_from('<Q', nor, o+0x50)[0], struct.unpack_from('<Q', nor, o+0x58)[0]
        parts[name] = (pid, start * 2048, size * 2048)
    manifest = {'variant': '70', 'bootrom': 'not emulated; captured BIT/BCT handoff',
                'cmac_key': 'zero (validated against both original stage2 digests)', 'inputs': {}, 'partitions': parts}
    def read(component, variant, filename):
        p = args.extracted / 'MMX2' / component / variant / 'default' / filename
        data = p.read_bytes()
        manifest['inputs'][str(p)] = hashlib.sha256(data).hexdigest()
        return data
    def install(name, data):
        _, off, size = parts[name]
        assert len(data) <= size, (name, len(data), size)
        nor[off:off+size] = b'\xff' * size
        nor[off:off+len(data)] = data
    # Validate the key and captured metadata before changing anything.
    for i, name in enumerate(('STAGE2_RECOVERY', 'STAGE2_PRIMARY')):
        pid, off, _ = parts[name]
        record = bct + 0x1544 + 0x18 + i * 29
        assert struct.unpack_from('<I', iram, record)[0] == pid
        assert cmac(nor[off:off+0x8310]) == iram[record+4:record+20]
    for which in ('primary', 'recovery'):
        raw = read('qb-'+which, '50', 'qb_'+which+'.img')
        n2, n1 = struct.unpack_from('<II', raw)
        assert 8+n1+n2 == len(raw)
        stage2 = raw[8:8+n2]
        stage1 = raw[8+n2:]
        # Nvidia image padding: 0x80 followed by zeros, aligned to AES block.
        stage2 += b'\x80' + bytes((-len(stage2)-1) % 16)
        assert len(stage2) == 0x8310
        install('STAGE1_'+which.upper(), stage1)
        install('STAGE2_'+which.upper(), stage2)
        i = 1 if which == 'primary' else 0
        record = bct + 0x1544 + 0x1c + i * 29
        iram[record:record+16] = cmac(stage2)
    for part, component, variant, name in [
        ('KERNEL_PRIMARY','mifs-stage1','70','mifs-stage1.img'),
        ('KERNEL_RECOVERY','eifs','50','eifs.img'),
        ('MAIN_STAGE2','mifs-stage2','70','mifs-stage2.img'),
        ('SYSTEM','efs-sys','70','efs-system.img'),
        ('PERSIST','efs-pers','70','efs-persist.img')]:
        raw = bytearray(read(component, variant, name))
        if part.startswith('KERNEL_'):
            import zlib
            raw[:8] = b'ANDROID!'
            assert zlib.crc32(raw[:0x7fc]) == struct.unpack_from('<I',raw,0x7fc)[0]
        install(part, raw)
    # BootROM successful-primary-loader status. BCT in IRAM is authoritative here.
    struct.pack_into('<I', iram, 0x40, 1)
    struct.pack_into('<I', iram, 0x58, 0)
    (args.output/'nor.bin').write_bytes(nor)
    (args.output/'iram.bin').write_bytes(iram)
    # Preserve the supplied partition layout; replace its app partition only.
    with args.base_emmc.open('rb') as f:
        mbr = f.read(512)
    assert mbr[510:512] == b'\x55\xaa'
    assert mbr[450] in (0xb1, 0xb2)
    start, sectors = struct.unpack_from('<II',mbr,454)
    app = args.extracted/'MMX2/app/70/default/app.img'
    assert app.stat().st_size <= sectors*512
    # Preserve sparse zero ranges without modifying the source disk.
    copy_sparse(args.base_emmc, args.output/'emmc.raw')
    digest = hashlib.sha256()
    with app.open('rb') as src, (args.output/'emmc.raw').open('r+b') as dst:
        # The supplied experimental disk labels app as type 178; fstab expects 177.
        dst.seek(450)
        dst.write(b'\xb1')
        # QNX locates the second superblock relative to the partition end.
        assert app.stat().st_size % 512 == 0
        dst.seek(458)
        dst.write(struct.pack('<I', app.stat().st_size // 512))
        dst.seek(466)
        dst.write(b'\xb2')
        dst.seek(start*512)
        while chunk := src.read(8*1024**2):
            digest.update(chunk)
            dst.write(chunk)
    manifest['inputs'][str(app)] = digest.hexdigest()
    manifest['notes'] = ['BCT, partition table, splash, SWDL and other eMMC partitions originate from captures.',
                         'NOR-resident BCT is unchanged; this harness seeds updated BCT in IRAM.',
                         'No BootROM, fuse or secure-boot emulation is claimed.']
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(args.output)

if __name__ == '__main__':
    main()

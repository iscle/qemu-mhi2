#!/usr/bin/env python3
"""Extract paired GLSL/Tegra programs from the user's Kanzi resource files.

Generated shader sources are local firmware data, never repository assets.
KZB stores source pairs followed by compiled pairs in material order. Reject
resources whose pair counts or shader-stage reflection disagree.
"""
import argparse
import json
from pathlib import Path
import re
import struct
from qnx6_read import Qnx6


def shader_hash(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def pairs(data):
    if len(data) < 0x4a or data[:4] != b'.KZB':
        raise ValueError('Not a KZB resource')
    # Directory records use network byte order; the ARM resource payloads use
    # little endian. Leaf reference IDs are one-based in directory order.
    directory_end = 0x3c + struct.unpack_from('>I', data, 0x34)[0]
    if directory_end > len(data):
        raise ValueError('KZB directory exceeds resource length')
    resources = [None]
    for pos in range(0x3c, directory_end - 18):
        offset, size, kind, flags, length = struct.unpack_from('>IIIIH', data, pos)
        name = data[pos+18:pos+18+length]
        if (directory_end <= offset < len(data) and size <= len(data)-offset
                and 0 < kind < 10000 and flags == 0 and 0 < length < 256
                and len(name) == length and all(32 <= c < 127 for c in name)):
            resources.append((offset, size, kind, name))
    result = []
    for match in re.finditer(rb'Tegra\x00\x00\x00\x00\x00', data):
        vs, fs, count = struct.unpack_from('<III', data, match.start()-14)
        if count != 1:
            raise ValueError('Unexpected compiled format count')
        pos = match.end()
        for stage, ref in enumerate((vs, fs)):
            if ref >= len(resources) or not resources[ref] or resources[ref][2] != 2010:
                raise ValueError('Invalid shader source reference')
            offset, source_size, _, name = resources[ref]
            source = data[offset+4:offset+4+source_size]
            size = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            binary = data[pos:pos+size]
            if len(binary) != size or size < 324 or binary[8:12] != bytes.fromhex('18000100'):
                raise ValueError('Invalid Tegra program')
            if b'void main' not in source:
                raise ValueError('Source unavailable')
            if (b'gl_Position' in source) != (stage == 0):
                raise ValueError('Shader stage mismatch')
            result.append((source, binary))
            pos += size
    return result


def blend_mode(source):
    match = re.search(rb'#pragma profilepragma blendoperation\(gl_FragColor,\s*(\w+),\s*(\w+),\s*(\w+)\)', source)
    if not match:
        return 0
    modes = {(b'GL_FUNC_ADD', b'GL_ONE', b'GL_ONE_MINUS_SRC_ALPHA'): 1,
             (b'GL_FUNC_ADD', b'GL_SRC_ALPHA', b'GL_ONE_MINUS_SRC_ALPHA'): 3,
             (b'GL_FUNC_ADD', b'GL_ONE', b'GL_ZERO'): 4,
             (b'GL_FUNC_ADD', b'GL_ZERO', b'GL_ZERO'): 5}
    return modes[match.groups()]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--app', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    fs = Qnx6(str(args.app), 0)
    args.output.mkdir(parents=True, exist_ok=True)
    report = {}; rejected = []
    def walk(ino=1, path=''):
        for name, child in fs.listdir(ino):
            if name in ('.', '..'): continue
            full = path+'/'+name
            if fs.is_dir(child):
                yield from walk(child, full)
            elif name.endswith('.kzb'):
                yield full, fs.read_file(child)
    for path, data in walk():
        try:
            for source, binary in pairs(data):
                mode = blend_mode(source)
                # Compile the original source using host fixed-function blending.
                source = re.sub(rb'^#pragma profilepragma blendoperation[^\n]*\n', b'', source, flags=re.M)
                name = f'{shader_hash(binary):016x}-{len(binary)}'
                if name not in report:
                    (args.output/(name+'.glsl')).write_bytes(source+b'\n')
                    (args.output/(name+'.blend')).write_text(str(mode))
                    report[name] = {'resource':path, 'blend':mode}
        except (ValueError, KeyError, struct.error) as exc:
            rejected.append({'resource':path, 'reason':str(exc)})
    (args.output/'manifest.json').write_text(json.dumps({'shaders':report,'rejected':rejected},indent=2)+'\n')
    print(f'{len(report)} shader programs; {len(rejected)} rejected resources')


if __name__ == '__main__':
    main()

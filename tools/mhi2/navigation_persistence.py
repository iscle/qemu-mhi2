#!/usr/bin/env python3
"""Add the native navigation enable preference to a captured persistence TOC.

Input: hex output of dumb_persistence_reader with version
'Navigation Persistence Manager', partition 1211, key 0. Output: guest writer
commands, preserving existing entries. This is a saved application preference;
it does not modify navigation code, map signatures, or FEC entitlements.
"""
import argparse
import shlex
import struct
import zlib


def u32(value):
    return struct.pack('<I', value)


def pack_blob(payload):
    data = u32(len(payload)) + zlib.compress(payload)
    return bytes((0, sum((i + 1) * b for i, b in enumerate(data)) & 255)) + data


def unpack_blob(data):
    if len(data) < 6 or data[0] != 0:
        raise ValueError('Unsupported persistence envelope')
    if sum((i + 1) * b for i, b in enumerate(data[2:])) & 255 != data[1]:
        raise ValueError('Persistence checksum mismatch')
    payload = zlib.decompress(data[6:])
    if len(payload) != struct.unpack_from('<I', data, 2)[0]:
        raise ValueError('Persistence length mismatch')
    return payload


def decode_toc(blob):
    data = unpack_blob(blob)
    pos = 0
    def integer():
        nonlocal pos
        result = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        return result
    if [integer(), integer(), integer()] != [0, 0xdeadbeef, 0xdeadbeef]:
        raise ValueError('Unsupported navigation TOC')
    entries = {}
    for _ in range(integer()):
        if integer() != 0:
            raise ValueError('Unsupported entry version')
        begin = pos
        while data[pos:pos + 2] != b'\0\0':
            if pos + 2 > len(data):
                raise ValueError('Unterminated entry name')
            pos += 2
        name = data[begin:pos].decode('utf-16le')
        pos += 2
        if integer() != 0:
            raise ValueError('Unsupported key-list version')
        entries[name] = [integer() for _ in range(integer())]
    used = [integer() for _ in range(integer())]
    if pos != len(data):
        raise ValueError('Unexpected trailing TOC data')
    return entries, used


def encode_toc(entries, used):
    data = u32(0) + u32(0xdeadbeef) * 2 + u32(len(entries))
    for name, keys in sorted(entries.items()):
        data += u32(0) + name.encode('utf-16le') + b'\0\0' + u32(0)
        data += u32(len(keys)) + b''.join(map(u32, keys))
    data += u32(len(used)) + b''.join(map(u32, used))
    return pack_blob(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('toc_hex')
    args = parser.parse_args()
    entries, used = decode_toc(bytes.fromhex(args.toc_hex))
    name = 'NavigationAppEnabledData'
    if name in entries:
        parser.error('Preference already exists; inspect its value before changing it')
    allocated = set(used) | {k for keys in entries.values() for k in keys} | {0}
    key = max(allocated) + 1
    entries[name] = [key]
    used.append(key)
    prefix = '/eso/bin/dumb_persistence_writer -f -v ' + shlex.quote('Navigation Persistence Manager') + ' 1211 '
    print(prefix + str(key) + ' ' + pack_blob(b'\1').hex())
    print(prefix + '0 ' + encode_toc(entries, used).hex())


if __name__ == '__main__':
    main()

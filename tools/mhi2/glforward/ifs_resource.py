"""Add an executable resource to an extracted little-endian QNX6 imagefs."""
import struct


def add_file(raw, name, payload):
    if raw[:7] != b'imagefs' or raw[7] != 4:
        raise ValueError('Unexpected IFS header')
    if not name or name.startswith('/') or '..' in name.split('/'):
        raise ValueError('Invalid IFS path')
    directory = struct.unpack_from('<I', raw, 16)[0]
    data_start = struct.unpack_from('<I', raw, 12)[0]
    pos, inode = directory, 0
    records = []
    while (length := struct.unpack_from('<H', raw, pos)[0]):
        if length < 24 or pos+length > data_start:
            raise ValueError('Invalid IFS directory')
        ino, mode = struct.unpack_from('<II', raw, pos+4)
        inode = max(inode, ino)
        name_offset = 32 if mode & 0xf000 == 0x8000 else 24
        existing = raw[pos+name_offset:pos+length].split(b'\0')[0]
        if existing == name.encode():
            raise ValueError('IFS resource already exists')
        records.append((pos, mode))
        pos += length
    # Retain original file alignment, including ELF mmap alignment.
    extra = (32+len(name.encode())+1+3)&~3
    shift = ((extra+4-(data_start-pos)+4095)//4096)*4096
    result = bytearray(raw[:data_start]+bytes(shift)+raw[data_start:-4])
    for offset, mode in records:
        if mode & 0xf000 == 0x8000:
            old = struct.unpack_from('<I', result, offset+24)[0]
            struct.pack_into('<I', result, offset+24, old+shift)
    result.extend(bytes((-len(result))%4096))
    payload_offset = len(result)
    result.extend(payload)
    result.extend(bytes((-len(result))%4)+bytes(4))
    record = bytearray(extra)
    struct.pack_into('<HHIIIII', record, 0, extra, 0, inode+1, 0o100755, 0, 0, 0)
    struct.pack_into('<II', record, 24, payload_offset, len(payload))
    record[32:32+len(name.encode())+1] = name.encode()+b'\0'
    result[pos:pos+extra+4] = record+bytes(4)
    struct.pack_into('<II', result, 8, len(result), data_start+shift)
    checksum = sum(struct.unpack('<%dI' % (len(result)//4), result)) & 0xffffffff
    struct.pack_into('<I', result, len(result)-4, (-checksum)&0xffffffff)
    return result

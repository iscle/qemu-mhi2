#!/usr/bin/env python3
"""Check bounded host EBO reads used to transfer guest client vertex arrays."""
import os
import struct
import subprocess


def u(*values):
    return struct.pack('<' + 'I' * len(values), *values)


with open('/tmp/mhi2-client-indices-check.log', 'wb') as log:
    process = subprocess.Popen([os.environ.get('MHI2_GLHOST', '/tmp/mhi2-glhost')],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=log)
    def send(op, data):
        process.stdin.write(u(op, len(data)) + data)
        process.stdin.flush()
    def maximum(count, kind, offset):
        send(128, u(count, kind, offset))
        return struct.unpack('<I', process.stdout.read(4))[0]
    send(1, u(32, 32))
    send(15, u(1, 1))
    send(16, u(0x8893, 1))
    for kind, fmt, width in [(0x1401, 'B', 1), (0x1403, 'H', 2), (0x1405, 'I', 4)]:
        values = struct.pack('<4' + fmt, 250, 3, 12, 7)
        send(17, u(0x8893, len(values), 0x88e4) + values)
        assert maximum(3, kind, width) == 12
        assert maximum(4, kind, 0) == 250
        assert maximum(4, kind, width) == 0xffffffff
        assert maximum(1, kind, 0xffffffff) == 0xffffffff
    process.stdin.close()
    assert process.wait(timeout=10) == 0
print('Byte, short and integer index ranges and out-of-bounds reads passed')

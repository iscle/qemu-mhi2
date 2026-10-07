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
    def maximum(count, kind, offset, opcode=138):
        send(opcode, u(count, kind, offset))
        return struct.unpack('<I', process.stdout.read(4))[0]
    send(1, u(32, 32))
    send(15, u(1, 1))
    send(16, u(0x8893, 1))
    for kind, fmt, width in [(0x1401, 'B', 1), (0x1403, 'H', 2), (0x1405, 'I', 4)]:
        values = struct.pack('<4' + fmt, 250, 3, 12, 7)
        send(17, u(0x8893, len(values), 0x88e4) + values)
        assert maximum(3, kind, width) == 12
        assert maximum(4, kind, 0) == 250
        assert maximum(4, kind, 0, opcode=128) == 250
        assert maximum(4, kind, width) == 0xffffffff
        assert maximum(1, kind, 0xffffffff) == 0xffffffff
    # Porsche client arrays and Audi EBO queries must coexist in one stream.
    vertex = b'attribute vec2 pos;void main(){gl_Position=vec4(pos,0.,1.);}'
    fragment = b'precision mediump float;void main(){gl_FragColor=vec4(.25,.5,.75,1.);}'
    for ident, kind, source in [(2, 0x8b31, vertex), (3, 0x8b30, fragment)]:
        send(7, u(ident, kind)); send(8, u(ident, len(source)) + source); send(9, u(ident))
    send(10, u(4)); send(11, u(4, 2)); send(11, u(4, 3))
    send(14, u(4, 0, 3) + b'pos'); send(12, u(4)); send(13, u(4))
    send(20, u(0)); send(2, u(0, 0, 32, 32))
    indices = struct.pack('<3H', 0, 1, 2)
    send(17, u(0x8893, len(indices), 0x88e4) + indices)
    assert maximum(3, 0x1403, 0) == maximum(3, 0x1403, 0, opcode=128) == 2
    vertices = struct.pack('<6f', -1, -1, 3, -1, -1, 3)
    send(128, u(0, 2, 0x1406, 0, 0, len(vertices)) + vertices)
    for opcode, payload in [(32, u(4, 3, 0x1403, 0)),
                            (129, u(4, 3, 0x1403, len(indices)) + indices)]:
        send(opcode, payload)
        send(120, u(3, 3, 1, 1, 0x1908, 0x1401))
        pixel = process.stdout.read(4)
        assert len(pixel) == 4 and all(abs(a-b) <= 1 for a, b in
                                      zip(pixel, (64, 128, 191, 255))), pixel
    process.stdin.close()
    assert process.wait(timeout=10) == 0
print('Index bounds, legacy/new query compatibility, client-array and EBO pixels passed')

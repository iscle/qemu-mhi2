#!/usr/bin/env python3
"""Check KZB source references independently of physical source payload order."""
import struct
from extract_kanzi_shaders import pairs, blend_mode

vertex = b'void main(){gl_Position=vec4(0.0);}'
fragment = b'void main(){gl_FragColor=vec4(1.0);}'
data = bytearray(8192)
data[:4] = b'.KZB'
struct.pack_into('>I', data, 0x34, 0x200 - 0x3c)
for directory, offset, source, name in [
        (0x3c, 0x800, vertex, b'Vertex Shader'),
        (0x80, 0x400, fragment, b'Fragment Shader')]:
    struct.pack_into('>IIIIH', data, directory, offset, len(source), 2010, 0, len(name))
    data[directory+18:directory+18+len(name)] = name
    data[offset+4:offset+4+len(source)] = source
binary = bytearray(324)
binary[8:12] = bytes.fromhex('18000100')
record = (struct.pack('<IIIH', 1, 2, 1, 5) + b'Tegra' + bytes(5) +
          struct.pack('<I', len(binary)) + binary +
          struct.pack('<I', len(binary)) + binary)
data[0xc00:0xc00+len(record)] = record
result = list(pairs(data))
assert [source for source, compiled in result] == [vertex, fragment]
assert all(compiled == binary for source, compiled in result)
struct.pack_into('<I', data, 0xc00, 2)
try:
    list(pairs(data))
except ValueError:
    pass
else:
    raise AssertionError('Accepted a fragment source for a vertex shader')
for src, dst, expected in [('GL_ONE', 'GL_ONE_MINUS_SRC_ALPHA', 1),
                           ('GL_SRC_ALPHA', 'GL_ONE_MINUS_SRC_ALPHA', 3),
                           ('GL_ONE', 'GL_ZERO', 4), ('GL_ZERO', 'GL_ZERO', 5)]:
    pragma = f'#pragma profilepragma blendoperation(gl_FragColor, GL_FUNC_ADD, {src}, {dst})\n'
    assert blend_mode(pragma.encode()) == expected
print('KZB reference order, shader stages and baked blending passed')

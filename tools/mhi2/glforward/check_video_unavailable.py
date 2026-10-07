#!/usr/bin/env python3
"""Non-Apple hosts must reject native video instead of promising blank frames."""
import os
import struct
import subprocess
import sys

if sys.platform == 'darwin':
    raise SystemExit('Use host/check_video_decoder.c for the VideoToolbox backend')


def packet(opcode, payload):
    return struct.pack('<II', opcode, len(payload)) + payload


stream = b''.join([
    packet(134, struct.pack('<III', 1, 800, 480)),
    packet(135, struct.pack('<II', 1, 4) + b'\0\0\0\1'),
    packet(136, struct.pack('<I', 1) + bytes(64)),
    packet(137, struct.pack('<I', 1)),
])
result = subprocess.run([os.environ.get('MHI2_GLHOST', '/tmp/mhi2-glhost')],
                        input=stream, capture_output=True, check=True, timeout=5)
assert result.stdout == struct.pack('<4I', 0x80000003, 0x80000003, 0x80000003, 0)
print('Unavailable native decoder rejects open/decode/attributes; close is harmless')

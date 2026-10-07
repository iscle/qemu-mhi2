#!/usr/bin/env python3
"""Exercise the LZOZ media codec, including pkg-config library discovery."""
from unittest.mock import patch
from lzo_library import load_lzo2
from prepare_ui import pack_lzo

# Test loading via pkg-config even on systems where ldconfig normally finds it.
with patch('lzo_library.ctypes.util.find_library', return_value=None):
    lib = load_lzo2()
    assert lib.lzo1z_decompress_safe
# pack_lzo verifies every compressed block against the original input.
packed = pack_lzo(bytes(range(256)) * 17000)
assert packed[:4] == b'LZOZ'
print('pkg-config LZO discovery and multi-block LZOZ codec round trip passed')

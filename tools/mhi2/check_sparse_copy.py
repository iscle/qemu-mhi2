#!/usr/bin/env python3
"""Check media copies, including holes, partial blocks and exclusive creation."""
from pathlib import Path
import tempfile
from sparse_copy import copy_sparse

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    source, dest = root/'source', root/'dest'
    for data in (b'', b'\0' * 19,
                 bytes(4 * 1024 * 1024) + b'firmware' + bytes(5 * 1024 * 1024)):
        source.write_bytes(data)
        copy_sparse(source, dest)
        assert dest.read_bytes() == data
        try:
            copy_sparse(source, dest)
        except FileExistsError:
            pass
        else:
            raise AssertionError('Overwrote existing media')
        assert source.read_bytes() == dest.read_bytes() == data
        dest.unlink()
print('Sparse image contents, trailing holes and existing-file protection passed')

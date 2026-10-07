"""Copy a disk image without GNU cp flags or allocating its zero-filled ranges."""
from pathlib import Path


def copy_sparse(source, destination):
    """Create destination exclusively, preserving bytes and trailing holes."""
    with Path(source).open('rb') as src, Path(destination).open('xb') as dst:
        while chunk := src.read(4 * 1024 * 1024):
            if chunk.count(0) == len(chunk):
                dst.seek(len(chunk), 1)
            else:
                dst.write(chunk)
        dst.truncate()

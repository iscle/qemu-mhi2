"""Copy a disk image exclusively, marking skipped zero ranges sparse on Windows."""
import os
from pathlib import Path


def windows_ioctl(stream, code, data=None):
    if os.name != 'nt':
        return
    import ctypes
    from ctypes import wintypes
    import msvcrt
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    ioctl = kernel.DeviceIoControl
    ioctl.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID,
                     wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                     ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID)
    ioctl.restype = wintypes.BOOL
    returned = wintypes.DWORD()
    if not ioctl(msvcrt.get_osfhandle(stream.fileno()), code,
                 ctypes.byref(data) if data is not None else None,
                 ctypes.sizeof(data) if data is not None else 0,
                 None, 0, ctypes.byref(returned), None):
        raise ctypes.WinError(ctypes.get_last_error())


def mark_sparse(stream):
    """Mark a new Windows file sparse; skipping writes alone is insufficient."""
    windows_ioctl(stream, 0x900c4)  # FSCTL_SET_SPARSE


def release_zero_range(stream, start, end):
    if os.name == 'nt':
        import ctypes
        class ZeroRange(ctypes.Structure):
            _fields_ = [('start', ctypes.c_int64), ('end', ctypes.c_int64)]
        windows_ioctl(stream, 0x980c8, ZeroRange(start, end))  # FSCTL_SET_ZERO_DATA


def copy_stream_sparse(src, destination):
    """Exclusive output; fail closed if sparse marking is unavailable."""
    with Path(destination).open('xb') as dst:
        mark_sparse(dst)
        holes = []
        while chunk := src.read(4 * 1024 * 1024):
            if chunk.count(0) == len(chunk):
                start = dst.tell()
                dst.seek(len(chunk), 1)
                if os.name == 'nt':
                    if holes and holes[-1][1] == start:
                        holes[-1] = (holes[-1][0], dst.tell())
                    else:
                        holes.append((start, dst.tell()))
            else:
                dst.write(chunk)
        dst.truncate()
        dst.flush()
        for start, end in holes:
            release_zero_range(dst, start, end)


def copy_sparse(source, destination):
    """Create destination exclusively, preserving bytes and trailing holes."""
    with Path(source).open('rb') as src:
        copy_stream_sparse(src, destination)

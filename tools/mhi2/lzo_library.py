"""Locate liblzo2 on system paths and in pkg-config prefixes (e.g. Homebrew)."""
import ctypes
import ctypes.util
from pathlib import Path
import subprocess
import sys


def load_lzo2():
    name = ctypes.util.find_library('lzo2')
    if name:
        try:
            return ctypes.CDLL(name)
        except OSError:
            pass
    # Apple's loader search does not include Homebrew's Apple Silicon prefix.
    # Ask the installed package rather than hardcoding /opt/homebrew or /usr/local.
    try:
        prefix = subprocess.check_output(
            ['pkg-config', '--variable=libdir', 'lzo2'], text=True).strip()
        if prefix:
            suffix = 'dylib' if sys.platform == 'darwin' else 'so'
            return ctypes.CDLL(str(Path(prefix)/f'liblzo2.{suffix}'))
    except (OSError, subprocess.CalledProcessError):
        pass
    raise OSError('liblzo2 not found; install liblzo2 (Linux) or brew install lzo (macOS)')

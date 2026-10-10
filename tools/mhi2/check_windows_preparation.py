"""Firmware-free checks for output admission and Windows sparse copying."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from prepare import app_partition
from sparse_copy import copy_sparse


class PreparationChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='mhi2-preparation-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.disk = self.root/'disk.raw'
        self.app = self.root/'MMX2/app/70/default/app.img'
        self.app.parent.mkdir(parents=True)
        header = bytearray(512)
        header[450] = 0xb1
        struct.pack_into('<II', header, 454, 1, 4)
        header[510:512] = b'\x55\xaa'
        self.disk.write_bytes(header + bytes(4*512))
        self.app.write_bytes(bytes(4*512))

    def test_exact_fit_and_smaller_image(self):
        self.assertEqual(app_partition(self.disk, self.app), (1, 4))
        self.app.write_bytes(bytes(512))
        self.assertEqual(app_partition(self.disk, self.app), (1, 4))

    def test_capacity_refusal_leaves_inputs_and_output_unchanged(self):
        self.app.write_bytes(bytes(5*512))
        before = [hashlib.sha256(p.read_bytes()).hexdigest() for p in (self.disk, self.app)]
        output = self.root/'new-output'
        command = [sys.executable, str(Path(__file__).with_name('prepare.py')),
                   '--extracted', str(self.root), '--base-nor', str(self.root/'unused-nor'),
                   '--base-iram', str(self.root/'unused-iram'), '--base-emmc', str(self.disk),
                   '--output', str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn('captured slot has', result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(before, [hashlib.sha256(p.read_bytes()).hexdigest() for p in (self.disk, self.app)])
        output.mkdir()
        marker = output/'existing.txt'
        marker.write_bytes(b'preserve this output')
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(marker.read_bytes(), b'preserve this output')

    def test_malformed_bounds_and_alignment(self):
        for header in (b'', bytes(512)):
            self.disk.write_bytes(header)
            with self.assertRaises(ValueError):
                app_partition(self.disk, self.app)
        header = bytearray(512)
        header[450] = 0xb1
        header[510:512] = b'\x55\xaa'
        struct.pack_into('<II', header, 454, 1, 100)
        self.disk.write_bytes(header + bytes(512))
        with self.assertRaisesRegex(ValueError, 'outside'):
            app_partition(self.disk, self.app)
        struct.pack_into('<II', header, 454, 1, 1)
        self.disk.write_bytes(header + bytes(512))
        self.app.write_bytes(b'odd')
        with self.assertRaisesRegex(ValueError, 'whole number'):
            app_partition(self.disk, self.app)

    def test_sparse_copy_bytes_trailing_hole_and_exclusive_destination(self):
        source = self.root/'source.raw'
        target = self.root/'copy.raw'
        block = 4*1024*1024
        payload = b'A'*block + bytes(2*block) + b'B'*block + bytes(block)
        source.write_bytes(payload)
        copy_sparse(source, target)
        self.assertEqual(target.read_bytes(), payload)
        self.assertEqual(source.read_bytes(), payload)
        with self.assertRaises(FileExistsError):
            copy_sparse(source, target)
        self.assertEqual(target.read_bytes(), payload)
        if os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            compressed_size = kernel.GetCompressedFileSizeW
            compressed_size.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD))
            compressed_size.restype = wintypes.DWORD
            upper = wintypes.DWORD()
            ctypes.set_last_error(0)
            lower = compressed_size(str(target), ctypes.byref(upper))
            if lower == 0xffffffff and ctypes.get_last_error():
                raise ctypes.WinError(ctypes.get_last_error())
            allocated = (upper.value << 32) | lower
            self.assertLess(allocated, len(payload))

    def test_failed_sparse_marking_preserves_partial_and_source(self):
        source = self.root/'source.raw'
        target = self.root/'failed-copy.raw'
        source.write_bytes(b'unchanged input')
        with patch('sparse_copy.mark_sparse', side_effect=OSError('sparse unsupported')):
            with self.assertRaisesRegex(OSError, 'sparse unsupported'):
                copy_sparse(source, target)
        self.assertEqual(source.read_bytes(), b'unchanged input')
        self.assertTrue(target.exists())
        self.assertEqual(target.stat().st_size, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)

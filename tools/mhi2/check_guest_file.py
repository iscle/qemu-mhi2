#!/usr/bin/env python3
"""Check binary console encoding and failure handling without booting QNX."""

import argparse
import base64
import contextlib
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import zlib

from guest_file import DECODER, destination_path, file_mode, upload


class DecoderTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('gawk'), 'requires GNU awk')
    def test_binary_and_padding(self):
        for payload in (b'', b'\0', b'\xff\0', bytes(range(256)),
                        bytes(range(256)) * 12, b'line\r\nnext\n\0end'):
            with self.subTest(size=len(payload)):
                result = subprocess.run(['gawk', '-v', 'token=CHECK', DECODER],
                                        input=base64.encodebytes(payload), capture_output=True,
                                        check=True, env={**os.environ, 'LC_ALL': 'C'})
                self.assertEqual(result.stdout, payload)
                self.assertEqual(result.stderr,
                                 f'CHECK:DATA {len(payload)} {zlib.adler32(payload)}\n'.encode())

    @unittest.skipUnless(shutil.which('gawk'), 'requires GNU awk')
    def test_invalid_input(self):
        result = subprocess.run(['gawk', DECODER], input=b'!!!\n', capture_output=True)
        self.assertNotEqual(result.returncode, 0)

    def test_paths_and_permissions(self):
        path = "/mnt/app/a b/'$(touch injected).bin"
        self.assertEqual(destination_path(path), path)
        for value in ('relative', '/', '/tmp/', '/tmp/../etc/file', '/tmp/a\nb'):
            with self.assertRaises(argparse.ArgumentTypeError):
                destination_path(value)
        self.assertEqual(file_mode('0755'), '0755')
        for value in ('888', '7777', '644; touch x'):
            with self.assertRaises(argparse.ArgumentTypeError):
                file_mode(value)

    def test_corruption_never_installs_destination(self):
        class CorruptConsole:
            def __init__(self):
                self.commands = []

            def execute(self, command, token=None):
                self.commands.append(command)
                if token:
                    # Report a valid-looking count with the wrong checksum.
                    return f'{token}:DATA 3 0\n'.encode()
                return b''

        console = CorruptConsole()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source'
            source.write_bytes(b'abc')
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
                    upload(console, source, '/tmp/destination')
        self.assertFalse(any(re.search(r'\bmv\b', command) for command in console.commands))


if __name__ == '__main__':
    unittest.main()

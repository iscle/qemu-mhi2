"""Firmware-free checks for experimental native host primitives."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from native_transport import FrameDecoder, SessionLock


class FrameDecoderTests(unittest.TestCase):
    def test_fragmented_and_combined_frames(self):
        frames = [b"a", b"synthetic Ethernet payload", b"z" * 65535]
        stream = b"".join(struct.pack("!I", len(frame)) + frame for frame in frames)
        decoder = FrameDecoder()
        actual = []
        for offset in range(0, len(stream), 17):
            actual.extend(decoder.feed(stream[offset : offset + 17]))
        decoder.finish()
        self.assertEqual(actual, frames)

    def test_rejects_bad_lengths_and_truncation(self):
        for length in (0, 65536, 0xFFFFFFFF):
            with self.subTest(length=length), self.assertRaises(ValueError):
                FrameDecoder().feed(struct.pack("!I", length))
        decoder = FrameDecoder()
        decoder.feed(struct.pack("!I", 5) + b"xy")
        with self.assertRaisesRegex(ValueError, "Truncated"):
            decoder.finish()
        with self.assertRaisesRegex(ValueError, "bound"):
            FrameDecoder().feed(b"x" * (2 * (65535 + 4) + 1))


class SessionLockTests(unittest.TestCase):
    def test_exclusion_and_reacquisition_across_processes(self):
        with tempfile.TemporaryDirectory() as temp:
            lock_path = Path(temp) / "session.lock"
            probe = """
import sys
sys.path.insert(0, sys.argv[1])
from native_transport import SessionLock
lock = SessionLock(sys.argv[2])
try:
    lock.acquire()
except RuntimeError:
    raise SystemExit(2)
else:
    lock.close()
"""
            command = [sys.executable, "-c", probe, str(HERE.parent), str(lock_path)]
            with SessionLock(lock_path):
                blocked = subprocess.run(command, capture_output=True, check=False)
                self.assertEqual(blocked.returncode, 2, blocked.stderr.decode(errors="replace"))
            reacquired = subprocess.run(command, capture_output=True, check=False)
            self.assertEqual(reacquired.returncode, 0, reacquired.stderr.decode(errors="replace"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
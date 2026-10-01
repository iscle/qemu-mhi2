#!/usr/bin/env python3
"""Replay a native MOST capture with a pause to verify H.264 state retention."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('capture', type=Path)
args = parser.parse_args()
capture = args.capture.read_bytes()
offset = 0
for _ in range(20):
    if offset + 4 > len(capture):
        parser.error('Capture must contain more than 20 complete native blocks')
    offset += 4 + struct.unpack_from('<I', capture, offset)[0]
if offset >= len(capture):
    parser.error('Capture must contain data after the first 20 blocks')
with tempfile.TemporaryDirectory(prefix='mhi2-pause-check-') as directory:
    root = Path(directory)
    source, frame = root/'blocks.bin', root/'frame.ppm'
    source.write_bytes(capture[:offset])
    with (root/'stderr').open('wb') as errors, (root/'stdout').open('wb') as status:
        process = subprocess.Popen([sys.executable, str(Path(__file__).with_name('most_sink.py')),
                                    '--input', str(source), '--output', str(frame)],
                                   stderr=errors, stdout=status)
        try:
            deadline = time.monotonic() + 15
            while not frame.exists() and time.monotonic() < deadline:
                time.sleep(.1)
            assert frame.exists(), 'No frame before pause'
            before = frame.stat().st_mtime_ns
            time.sleep(3.5)
            with source.open('ab') as output:
                output.write(capture[offset:])
            deadline = time.monotonic() + 15
            while frame.stat().st_mtime_ns == before and time.monotonic() < deadline:
                time.sleep(.1)
            assert frame.stat().st_mtime_ns > before, 'No frame after pause'
            time.sleep(1)
        finally:
            process.terminate()
            process.wait(timeout=10)
    errors = (root/'stderr').read_text()
    assert not errors, errors
    print('Native H.264 resumes after a 3.5-second pause: PASS')
    print((root/'stdout').read_text().strip())

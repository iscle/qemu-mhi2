#!/usr/bin/env python3
"""Ask a running Porsche K5126 firmware to connect using its native services."""
import argparse
import json
import os
from pathlib import Path
import stat
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('connection', choices=('wifi', 'cellular'))
    parser.add_argument('--ssid', default='QEMU Wi-Fi')
    parser.add_argument('--fifo', type=Path, default=Path('/tmp/mhi2-rcc-input'))
    args = parser.parse_args()
    if not 1 <= len(args.ssid.encode()) <= 32 or '\0' in args.ssid:
        parser.error('SSID must contain 1–32 UTF-8 bytes and no NUL')
    event = dict(type='network', action=args.connection, ssid=args.ssid)
    try:
        fd = os.open(args.fifo, os.O_WRONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        try:
            if not stat.S_ISFIFO(os.fstat(fd).st_mode):
                raise ValueError('The RCC input path must be a FIFO')
            os.write(fd, (json.dumps(event) + '\n').encode())
        finally:
            os.close(fd)
    except (OSError, ValueError) as exc:
        print(f'Cannot send network request: {exc}', file=sys.stderr)
        return 1
    print('Connection requested; inspect the firmware network status or '
          '/tmp/mhi2-rcc-peer.log for the result.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

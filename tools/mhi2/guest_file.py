#!/usr/bin/env python3
"""Copy a host file into a running MHI2 guest through its diagnostic console."""

import argparse
import base64
import errno
import fcntl
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import stat
import sys
import time
import uuid
import zlib


# QNX's gawk 3.1.5 truncates input strings at NUL, but printf %c can write
# every byte. Decode ASCII base64 and checksum the decoded byte values.
DECODER = r'''
function put(v) {
    printf "%c",v
    a=(a+v)%65521; b=(b+a)%65521; count++
}
BEGIN {
    a=1; s="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    for(i=1;i<=64;i++) d[substr(s,i,1)]=i-1
}
{
    if(length($0)%4 || $0 ~ /[^A-Za-z0-9+\/=]/) exit 1
    for(i=1;i<=length($0);i+=4) {
        c=substr($0,i+2,1); e=substr($0,i+3,1)
        n=d[substr($0,i,1)]*262144+d[substr($0,i+1,1)]*4096+d[c]*64+d[e]
        put(int(n/65536))
        if(c!="=") put(int(n/256)%256)
        if(e!="=") put(n%256)
    }
}
END { printf "%s:DATA %.0f %.0f\n", token,count,b*65536+a > "/dev/stderr" }
'''


class Console:
    def __init__(self, timeout):
        self.timeout = timeout
        self.fd = None
        self.log = None
        self.lock = open('/tmp/mhi2-file-transfer.lock', 'a')
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.log = open('/tmp/mhi2-debug-console.log', 'rb')
            if b'UI_DIAGNOSTIC_SHELL_READY' not in self.log.read():
                raise RuntimeError('Wait for UI_DIAGNOSTIC_SHELL_READY before uploading.')
            self.fd = os.open('/tmp/mhi2-shell-commands', os.O_WRONLY | os.O_NONBLOCK)
            if not stat.S_ISFIFO(os.fstat(self.fd).st_mode):
                raise RuntimeError('The diagnostic command path is not a FIFO.')
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
        if self.log is not None:
            self.log.close()
        self.lock.close()

    def execute(self, command, token=None):
        token = token or 'MHI2_FILE_' + uuid.uuid4().hex
        data = (command + '\nprint -r -- ' + token + ':STATUS:$?\n').encode('utf-8')
        deadline = time.monotonic() + self.timeout
        response = bytearray()
        pattern = re.compile(rb'(?:^|\n)' + token.encode() + rb':STATUS:(\d+)\r?\n')
        while time.monotonic() < deadline:
            if data:
                try:
                    sent = os.write(self.fd, data[:4096])
                    data = data[sent:]
                except BlockingIOError:
                    pass
            response.extend(self.log.read())
            match = pattern.search(response)
            if match:
                if int(match[1]):
                    raise RuntimeError('Guest command failed; see /tmp/mhi2-debug-console.log.')
                return bytes(response)
            time.sleep(0.02)
        raise TimeoutError('No guest acknowledgement. Check that the emulator is still running; '
                           'the destination has not been confirmed.')


def destination_path(value):
    path = PurePosixPath(value)
    if not path.is_absolute() or path.name in ('', '.', '..') or '..' in path.parts:
        raise argparse.ArgumentTypeError('Use an absolute guest filename without .. components.')
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise argparse.ArgumentTypeError('Guest paths cannot contain control characters.')
    if value.endswith('/'):
        raise argparse.ArgumentTypeError('Specify a filename, not a directory ending in /.')
    return str(path)


def file_mode(value):
    if not re.fullmatch(r'0?[0-7]{3}', value):
        raise argparse.ArgumentTypeError('Use an octal permission mode such as 0644 or 0755.')
    return value


def upload(console, source, destination, overwrite=False, mode='0644'):
    temporary = str(PurePosixPath(destination).parent / ('.mhi2-upload-' + uuid.uuid4().hex))
    print('Guest temporary files:', temporary + '.part', temporary + '.awk', flush=True)
    target = shlex.quote(destination)
    script = shlex.quote(temporary + '.awk')
    payload = shlex.quote(temporary + '.part')
    absent = f'[ ! -e {target} ] && [ ! -L {target} ]'
    guard = (f'( [ ! -e {target} ] || [ -f {target} ] ) && [ ! -L {target} ]'
             if overwrite else absent)
    # Check destination and utilities before creating anything. The temporary
    # files live beside the destination so the final rename stays local.
    # /tmp aliases QNX /dev/shmem, which permits files but not subdirectories.
    console.execute(f'{guard} && [ -x /armle/usr/bin/gawk ] && '
                    f'[ -x /armle/usr/bin/wc ] && '
                    f'(umask 077; set -C; : > {payload} && : > {script})')
    size = 0
    last_progress = time.monotonic()
    with source.open('rb') as stream:
        while block := stream.read(3072):
            token = 'MHI2_FILE_' + uuid.uuid4().hex
            # Keep the guest's interactive input lines short, including the
            # awk program itself: a heredoc installs that small script once.
            if size == 0:
                console.execute(f"cat > {script} <<'{token}'\n{DECODER}\n{token}")
            command = (f"LC_ALL=C /armle/usr/bin/gawk -v token={token} -f {script} "
                       f">> {payload} <<'{token}'\n" + base64.encodebytes(block).decode('ascii') + token)
            response = console.execute(command, token)
            expected = f'{token}:DATA {len(block)} {zlib.adler32(block)}'.encode()
            if not re.search(rb'(?:^|\n)' + re.escape(expected) + rb'\r?\n', response):
                raise RuntimeError('Guest block checksum mismatch; destination was not installed.')
            size += len(block)
            if time.monotonic() - last_progress >= 2:
                print(f'Transferred {size} bytes', flush=True)
                last_progress = time.monotonic()
    console.execute(f'[ "$(/armle/usr/bin/wc -c < {payload})" -eq {size} ] && '
                    f'chmod {mode} {payload}')
    console.execute(f'{guard} && mv -f {payload} {target}')
    console.execute(f'rm -f {script} && sync')
    print(f'Copied {size} bytes to {destination} (block checksums and guest file size verified).')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='Host file to upload')
    parser.add_argument('destination', type=destination_path, help='Absolute guest filename')
    parser.add_argument('--overwrite', action='store_true', help='Replace an existing guest file')
    parser.add_argument('--mode', type=file_mode, default='0644', help='Guest permissions (default: 0644)')
    parser.add_argument('--timeout', type=float, default=30, help='Seconds per acknowledgement (default: 30)')
    args = parser.parse_args()
    if not args.source.is_file() or args.timeout <= 0:
        parser.error('Source must be a regular file and timeout must be positive.')
    console = None
    try:
        console = Console(args.timeout)
        upload(console, args.source, args.destination, args.overwrite, args.mode)
    except (OSError, RuntimeError, KeyboardInterrupt) as exc:
        detail = 'No running emulator is reading the command FIFO.' if getattr(exc, 'errno', None) == errno.ENXIO else str(exc)
        print('Upload failed: ' + (detail or 'interrupted'), file=sys.stderr)
        print('Any remaining temporary files are at the printed guest paths for inspection. '
              'Do not send other shell commands until queued input has finished.', file=sys.stderr)
        return 1
    finally:
        if console is not None:
            console.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())

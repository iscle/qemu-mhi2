#!/usr/bin/env python3
"""Own the emulator helpers and desktop viewer as one local session."""
import argparse
import fcntl
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import time
from encoder_backend import configure_encoder
from firmware_profile import PROFILES, configure
from wireless import arguments as wireless_arguments


def stop(process):
    if process is None:
        return
    # Helpers inherit the harness's process group; clean up even if it failed.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--headless', action='store_true', help='Run without the desktop viewer')
    parser.add_argument('--firmware', choices=PROFILES, default=os.environ.get('MHI2_FIRMWARE', 'vw'))
    parser.add_argument('--media', help='Prepared media directory for the selected firmware')
    parser.add_argument('--wifi', default=os.environ.get('MHI2_WIFI', 'user'),
                        help='QEMU -nic backend options (default: user); "none" disables Wi-Fi')
    parser.add_argument('--wifi-ssid', default=os.environ.get('MHI2_WIFI_SSID', 'QEMU Wi-Fi'),
                        help='Name of the single emulated open access point')
    parser.add_argument('--bluetooth', default=os.environ.get('MHI2_BLUETOOTH', ''),
                        help='QEMU -chardev options for an H4 controller, without id=')
    parser.add_argument('--cellular', choices=('off', '3g', '4g'),
                        default=os.environ.get('MHI2_CELLULAR', 'off'),
                        help='Emulated Cinterion USB modem with QEMU NAT data')
    args = parser.parse_args()
    try:
        wireless_arguments(args.wifi, args.wifi_ssid, args.bluetooth, args.cellular)
        os.environ.update(MHI2_WIFI=args.wifi, MHI2_WIFI_SSID=args.wifi_ssid,
                          MHI2_BLUETOOTH=args.bluetooth, MHI2_CELLULAR=args.cellular)
        profile = configure(args.firmware, args.media)
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 1
    scripts = Path(__file__).resolve().parent
    with open('/tmp/mhi2-ui.lock', 'a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('An MHI2 session is already running.', file=sys.stderr)
            return 1
        for name in ('MHI2_DEBUG_NOR', 'MHI2_EMMC'):
            if not Path(os.environ[name]).is_file():
                print(f'Missing media: {os.environ[name]}', file=sys.stderr)
                return 1
        if not args.headless:
            try:
                import PySide6  # noqa: F401
            except ImportError:
                print('The viewer requires PySide6 in this Python environment.', file=sys.stderr)
                return 1
        try:
            print(configure_encoder(), flush=True)
        except (ValueError, RuntimeError) as exc:
            print(exc, file=sys.stderr)
            return 1
        Path('/tmp/glhost_frame.ppm').unlink(missing_ok=True)
        # The lock makes this session the sole owner of these transports.
        # Old FIFO descriptors can retain a partial graphics record or a QMP
        # quit command after a forced shutdown. Start with fresh pipe objects.
        for stem in ('mhi2-gl', 'mhi2-rcc', 'mhi2-debug-qmp'):
            for suffix in ('.in', '.out'):
                path = Path('/tmp') / (stem + suffix)
                if path.exists():
                    if not stat.S_ISFIFO(path.lstat().st_mode):
                        raise ValueError(f'Expected session FIFO: {path}')
                    path.unlink()
                os.mkfifo(path, 0o600)
        vm = viewer = None
        def interrupted(signum, frame):
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, interrupted)
        try:
            with open('/tmp/mhi2-session.log', 'w') as vm_log, open('/tmp/mhi2-viewer.log', 'w') as viewer_log:
                vm = subprocess.Popen([sys.executable, str(scripts/'debug_shell_local.py')],
                                      stdout=vm_log, stderr=subprocess.STDOUT, start_new_session=True)
                if not args.headless:
                    viewer = subprocess.Popen([sys.executable, str(scripts/'view_ui.py')],
                                              stdout=viewer_log, stderr=subprocess.STDOUT, start_new_session=True)
                print(profile['title'] + ' starting. Boot may take several minutes.', flush=True)
                print('Close the viewer or press Ctrl-C here to stop. Logs: /tmp/mhi2-session.log', flush=True)
                while vm.poll() is None:
                    if viewer is not None and viewer.poll() is not None:
                        if viewer.returncode:
                            print(Path('/tmp/mhi2-viewer.log').read_text()[-4000:], file=sys.stderr)
                            return 1
                        return 0
                    time.sleep(0.2)
                if vm.returncode:
                    print('Emulator exited; see /tmp/mhi2-session.log and /tmp/mhi2-debug-host.log.', file=sys.stderr)
                return int(vm.returncode != 0)
        except KeyboardInterrupt:
            return 0
        finally:
            stop(viewer)
            stop(vm)


if __name__ == '__main__':
    sys.exit(main())

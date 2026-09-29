#!/usr/bin/env python3
"""Check the real primary QNX header, userspace serial output, reset and CMAC rejection."""
import argparse
import json
import os
import selectors
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('media', type=Path)
    ap.add_argument('--qemu', type=Path, default=Path(__file__).resolve().parents[2]/'build/qemu-system-arm')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = {}
    for negative in [False, True]:
        name = 'corrupt-stage2' if negative else 'primary-reset'
        serial = args.output/(name+'.uart.log')
        nor = args.media/'nor.bin'
        with tempfile.TemporaryDirectory(prefix='mhi2-check-') as tmp:
            if negative:
                bad = Path(tmp)/'nor.bin'
                shutil.copyfile(nor,bad)
                with bad.open('r+b') as f:
                    for offset in [0xa0100,0x120100]:
                        f.seek(offset);v=f.read(1);f.seek(offset);f.write(bytes([v[0]^1]))
                nor = bad
            cmd = [str(args.qemu),'-M','mhi2-harman,iram='+str(args.media/'iram.bin'),
                   '-bios',str(nor),'-display','none','-monitor','none','-qmp','stdio',
                   '-serial','null','-serial','null','-serial','null','-serial','file:'+str(serial)]
            with (args.output/(name+'.host.log')).open('w') as err:
                p = subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,bufsize=0)
                pending = bytearray()
                def receive(timeout=10):
                    deadline=time.monotonic()+timeout
                    while b'\n' not in pending:
                        with selectors.DefaultSelector() as sel:
                            sel.register(p.stdout,selectors.EVENT_READ)
                            assert sel.select(max(0,deadline-time.monotonic())), 'QMP response timeout'
                        chunk=os.read(p.stdout.fileno(),65536)
                        assert chunk,'QEMU exited'
                        pending.extend(chunk)
                    line,_,rest=pending.partition(b'\n');pending[:]=rest
                    return json.loads(line)
                def q(command,arguments=None):
                    p.stdin.write((json.dumps({'execute':command,'arguments':arguments or {}})+'\n').encode())
                    while True:
                        r=receive()
                        if 'return' in r:return r['return']
                        assert 'error' not in r,r
                try:
                    receive();q('qmp_capabilities')
                    def wait_marker(marker,count=1,seconds=25):
                        deadline=time.monotonic()+seconds
                        while time.monotonic()<deadline:
                            text=serial.read_text(errors='replace') if serial.exists() else ''
                            if text.count(marker)>=count:return text
                            assert p.poll() is None,'QEMU exited'
                            time.sleep(.1)
                        raise AssertionError(f'Missing serial marker {marker!r}')
                    if negative:
                        wait_marker('Error in Loading Stage2')
                        results[name]='CMAC mismatch rejected before QNX handoff'
                    else:
                        wait_marker('Starting i2c interface for PMU')
                        q('stop')
                        header=q('human-monitor-command',{'command-line':'xp /14wx 0x80a00808','cpu-index':0})
                        # Primary image stored_size; recovery is larger.
                        assert '0x006c53cc' in header,header
                        regs=q('human-monitor-command',{'command-line':'info registers','cpu-index':0})
                        results['primary-header']=header
                        results['cpu0-after-userspace-start']=regs.split('s00=')[0]
                        q('system_reset');q('cont')
                        wait_marker('Starting i2c interface for PMU',count=2)
                        results[name]='Primary QNX userspace reached twice, including system_reset'
                    q('quit')
                finally:
                    if p.poll() is None:p.terminate()
                    try:p.wait(timeout=5)
                    except subprocess.TimeoutExpired:p.kill();p.wait()
    (args.output/'checks.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2))

if __name__=='__main__':
    main()

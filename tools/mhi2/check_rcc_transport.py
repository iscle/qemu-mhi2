#!/usr/bin/env python3
"""Stall the RCC peer and verify descriptor ownership and independent audio DMA."""
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile

media = Path(sys.argv[1]).resolve()
qemu = Path(__file__).resolve().parents[2] / 'build/qemu-system-arm'
scratch = tempfile.TemporaryDirectory(prefix='mhi2-rcc-test-')
pipe = Path(scratch.name) / 'rcc'
for suffix in ('.in', '.out'):
    os.mkfifo(str(pipe) + suffix)
rx = os.open(str(pipe) + '.in', os.O_RDWR | os.O_NONBLOCK)
tx = os.open(str(pipe) + '.out', os.O_RDWR | os.O_NONBLOCK)
most = Path(scratch.name) / 'most'
for suffix in ('.in', '.out'): os.mkfifo(str(most) + suffix)
most_rx = os.open(str(most)+'.in', os.O_RDWR | os.O_NONBLOCK)
most_tx = os.open(str(most)+'.out', os.O_RDWR | os.O_NONBLOCK)
p = subprocess.Popen([str(qemu), '-M', 'mhi2-harman,iram=' + str(media/'iram.bin'),
    '-bios', str(media/'nor.bin'), '-display', 'none', '-serial', 'null',
    '-monitor', 'none', '-accel', 'qtest', '-qtest', 'stdio',
    '-chardev', f'pipe,id=rcceth,path={pipe}',
    '-chardev', f'pipe,id=mostvideo,path={most}'], stdin=subprocess.PIPE,
    stdout=subprocess.PIPE, stderr=open('/tmp/mhi2-rcc-transport.log', 'w'), bufsize=0)
pending = bytearray()
def q(command):
    p.stdin.write((command + '\n').encode())
    while b'\n' not in pending:
        assert select.select([p.stdout], [], [], 5)[0], command
        block = os.read(p.stdout.fileno(), 4096)
        assert block
        pending.extend(block)
    line, _, rest = pending.partition(b'\n'); pending[:] = rest
    assert line.startswith(b'OK'), line
    return line.split()[1:]
def read(addr): return int(q(f'readl {addr:#x}')[0], 16)
def write(addr, value): q(f'writel {addr:#x} {value:#x}')
def step(): q('clock_step 1000000')
try:
    write(0x1018, 0x100)
    cfg, app, shared, ram = 0x01010000, 0x20000000, 0x20004000, 0x81000000
    assert read(cfg) == 0xb800104c
    write(cfg+0x10, app); write(cfg+0x14, shared)
    write(app+0x200, ram | 1)
    write(app+0x7c, 0xdeadbeef)
    write(shared+4, 2)
    payload = bytes(range(256)) * 8
    q(f'write {ram:#x} {len(payload)} 0x{payload.hex()}')
    for i in range(32):
        desc = shared + 0x3c8 + 28*i
        write(desc, 0xc0000000)
        write(desc+4, len(payload)); write(desc+8, len(payload))
        write(desc+16, 0x80000001)
    # Do not read the pipe: the previous synchronous implementation hangs
    # inside this clock_step while holding QEMU's global lock.
    step()
    assert read(shared+0x3c8+16) == 0x40000001
    # Completed DMA buffers belong to the guest again. Queued bytes must
    # survive their reuse, rather than referring back to this RAM.
    write(ram, 0xdeadbeef)
    write(0x6000a000, 1 << 31)
    write(0x70080000, 0x80070000); write(0x70080210, 1)
    write(0x70080300, 0xc0001207)
    write(0x6000b010, ram); write(0x6000b018, 0x7008000c)
    write(0x6000b000, 0xd800001c)
    step()
    assert read(0x6000b004) & (1 << 30), 'RCC peer stalled audio DMA'
    expected = (struct.pack('>I', len(payload)) + payload) * 32
    result = bytearray()
    for _ in range(1000):
        step()
        while select.select([tx], [], [], 0)[0]:
            result.extend(os.read(tx, 65536))
        if len(result) == len(expected): break
    assert result == expected, 'RCC output lost packet boundaries or DMA ownership'
    q(f'write {ram:#x} {len(payload)} 0x{payload.hex()}')
    # Fill the bounded queue. Outstanding descriptors remain owned by the
    # endpoint until the host drains enough bytes to accept their packet.
    blocked = False
    for batch in range(40):
        for i in range(32): write(shared+0x3c8+28*i+16, 0x80000001)
        step()
        if any(read(shared+0x3c8+28*i+16) & 0x80000000 for i in range(32)):
            blocked = True
            break
    assert blocked, 'RCC output queue grew without applying backpressure'
    expected = (struct.pack('>I',len(payload))+payload) * ((batch+1)*32)
    result = bytearray()
    for _ in range(3000):
        step()
        while select.select([tx],[],[],0)[0]: result.extend(os.read(tx,65536))
        if len(result)==len(expected): break
    assert result==expected, 'RCC queue wrap/resume corrupted frames'
    assert all(read(shared+0x3c8+28*i+16)==0x40000001 for i in range(32))
    # MOST accepts two maximum-sized blocks and holds the third descriptor.
    bar3 = 0x20009000
    write(cfg+0x1c,bar3)
    block=bytes(range(256))*4096
    for i in range(3):
        addr=ram+(i+1)*len(block)
        q(f'write {addr:#x} {len(block)} 0x{block.hex()}')
        write(bar3+(3+8)*4,0xc0000000+(i+1)*len(block))
        write(bar3+(3+16)*4,len(block))
        write(bar3+3*4,0x22222222)
        step()
        assert read(addr)==0x03020100, 'MOST consumed a descriptor before its doorbell'
        write(app+0x54,5)
        if i<2: assert read(addr)==0
    assert read(bar3+12)==0x22222222 and read(addr)==0x03020100
    step()
    result=bytearray(); expected=(struct.pack('<I',len(block))+block)*3
    for _ in range(3000):
        step()
        while select.select([most_tx],[],[],0)[0]: result.extend(os.read(most_tx,65536))
        if len(result)==len(expected): break
    assert result==expected, 'MOST packet ownership/order/backpressure failed'
    assert read(bar3+12)==0 and read(addr)==0
    print('PASS: bounded RCC/MOST output, queue wrap/resume, descriptor ownership, ordered packets, independent audio DMA')
finally:
    os.close(rx); os.close(tx)
    os.close(most_rx); os.close(most_tx)
    p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
    scratch.cleanup()

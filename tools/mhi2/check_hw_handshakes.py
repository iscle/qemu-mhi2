#!/usr/bin/env python3
"""Exercise MC flush completion and IOC request handshakes without executing firmware."""
import os
from pathlib import Path
import select
import subprocess
import sys

media = Path(sys.argv[1]).resolve()
qemu = Path(__file__).resolve().parents[2]/'build/qemu-system-arm'
p = subprocess.Popen([str(qemu), '-M', 'mhi2-harman,iram='+str(media/'iram.bin'),
    '-bios', str(media/'nor.bin'), '-display', 'none', '-serial', 'null',
    '-monitor', 'none', '-accel', 'qtest', '-qtest', 'stdio'], stdin=subprocess.PIPE,
    stdout=subprocess.PIPE, stderr=open('/tmp/mhi2-hw-handshakes.log', 'w'),
    bufsize=0, env=dict(os.environ, TMPDIR=str(media/'tmp')))
pending = bytearray()
def command(s):
    p.stdin.write((s+'\n').encode())
    while b'\n' not in pending:
        assert select.select([p.stdout], [], [], 5)[0], s
        block = os.read(p.stdout.fileno(), 4096)
        assert block
        pending.extend(block)
    line, _, rest = pending.partition(b'\n'); pending[:] = rest
    assert line.startswith(b'OK'), line
    return line.split()[1:]
def read(address): return int(command(f'readl {address:#x}')[0], 16)
def write(address, value): command(f'writel {address:#x} {value:#x}')
try:
    # CAR drives the A9 system timer. Check both board crystals, PLL/divider
    # changes, gated clocks, and a comparator deadline after a rate change.
    car, gt = 0x60006000, 0x50040200
    write(gt+8, 0)
    write(gt, 0); write(gt+4, 0)
    write(car+0xe0, 0x40025806)  # PLLX: N=600, M=6, P=0
    write(car+0x368, 0x20008888)
    write(gt+8, 1)
    def ticks(ns):
        before = read(gt)
        command(f'clock_step {ns}')
        return (read(gt) - before) & 0xffffffff
    assert abs(ticks(1000000) - 650000) <= 1, '13 MHz PLLX timer rate'
    before = read(gt)
    write(car+0x50, 0x800003f1)
    assert read(gt) == before, 'Clock switch jumped the counter'
    assert abs(ticks(1000000) - 600000) <= 1, '12 MHz PLLX timer rate'
    write(car+0xe0, 0x40125806)
    assert abs(ticks(1000000) - 300000) <= 1, 'PLLX post divider'
    write(car+0xe0, 0x00125806)
    assert ticks(1000000) == 0, 'Disabled PLL advanced the timer'
    write(car+0xe0, 0x80000000)
    assert abs(ticks(1000000) - 6000) <= 1, 'PLLX bypass'
    write(car+0x20, 0x10000000)  # legacy CCLK alias, IDLE CLK_M
    write(car+0x24, 2 << 16)    # U7.1 divide by two
    assert abs(ticks(1000000) - 3000) <= 1, 'CCLKG source/divider'
    write(gt+8, 0)
    assert ticks(1000000) == 0, 'Disabled timer advanced'
    write(gt, 0); write(gt+4, 0)
    write(car+0x50, 0x3f1)
    write(car+0xe0, 0x40025806)
    write(car+0x368, 0x20008888)
    write(gt+16, 649999)
    write(gt+20, 0)
    write(gt+8, 7)
    command('clock_step 999000')
    assert read(gt+12) == 0, 'Timer comparator fired early'
    command('clock_step 1000')
    assert read(gt+12) == 1, 'Timer comparator missed 1 ms deadline'
    # The native OS uses a 649999-cycle interval, not a power of two.
    # Delayed servicing must preserve the comparator's original phase.
    write(gt+8, 0)
    write(gt, 0); write(gt+4, 0)
    write(gt+12, 1)
    write(gt+16, 649999); write(gt+20, 0)
    write(gt+24, 649999)
    write(gt+8, 15)
    for ns in (1000000, 4000000, 17300000, 1000000):
        command(f'clock_step {ns}')
        counter = read(gt)
        compare = read(gt+16)
        assert compare == (counter // 649999 + 1) * 649999, \
            ('Non-power-of-two auto-increment lost phase', counter, compare)
        assert read(gt+12) == 1
        write(gt+12, 1)
        assert read(gt+12) == 0, 'Timer acknowledgement reasserted a past tick'
    write(gt+8, 0)
    write(gt+12, 1)
    # Native TDM DMA must move data before asserting completion, and must
    # preserve pending status while interrupt masking or pausing a channel.
    dma, chan, bridge, ram = 0x6000a000, 0x6000b000, 0x70080e00, 0x81000000
    assert read(bridge) == 0x4d484941
    write(dma, 1 << 31)
    write(ram+28, (2000 << 16) | 1000)
    write(chan+16, ram)
    write(chan+24, 0x7008000c)
    write(chan, 0xd800001c)
    command('clock_step 1000000')
    assert not read(chan+4) & (1 << 30), 'DMA ran with AHUB disabled'
    assert read(chan+4) & 0xfffc == 28, 'Unstarted DMA residual count'
    write(0x70080000, 0xc0070700)
    write(0x70080200, 1 << 4)
    write(0x70080210, 1)
    command('clock_step 1000000')
    assert not read(chan+4) & (1 << 30), 'DMA ran with I2S disabled'
    write(0x70080004, 0xc0000000)
    assert read(0x70080004) == 0, 'AHUB FIFO reset did not complete'
    write(0x70080300, 0x10001207)
    assert read(0x70080300) == 0x1207, 'I2S reset did not complete'
    write(0x70080300, 0xc0001207)
    command('clock_step 1000000')
    assert read(chan+4) & (1 << 30), 'DMA completion missing'
    assert not read(chan) & (1 << 31), 'One-shot DMA remained enabled'
    assert read(dma+0x14) & 1 and not read(dma+0x18) & 1
    write(dma+0x20, 1)
    assert read(dma+0x18) & 1, 'Unmask lost pending DMA interrupt'
    assert read(bridge+4) == 1 and read(bridge+8) == 3000
    write(chan+4, 1 << 30)
    assert not read(dma+0x14) & 1
    write(bridge+16, 1234)
    write(chan+16, ram+4096)
    write(chan+24, 0x70080010)
    write(chan+12, 1 << 31)
    write(chan, 0xc800001c)
    command('clock_step 1000000')
    assert not read(chan+4) & (1 << 30), 'Paused DMA transferred data'
    write(chan+12, 0)
    command('clock_step 1000000')
    assert read(ram+4096+16) >> 16 == 1234
    assert read(ram+4096+20) >> 16 == 1234
    assert read(chan+4) & (1 << 30)
    write(chan+4, 1 << 30)
    write(chan, 0xc000001c)
    command('clock_step 21000')
    assert not read(chan+4) & (1 << 28), 'RX first cyclic half has wrong direction polarity'
    command('clock_step 1000000')
    assert not read(chan+4) & (1 << 28), 'Unacknowledged half-buffer was overwritten'
    write(chan+4, 1 << 30)
    command('clock_step 21000')
    assert read(chan+4) & (1 << 28), 'RX second cyclic half has wrong direction polarity'
    write(chan, 0)
    # A mid-transfer pause freezes the residual count and resumes the
    # remaining samples; it must not restart a whole buffer period.
    write(chan+4, 1 << 30)
    write(chan+16, ram)
    write(chan+24, 0x7008000c)
    write(chan, 0xd80003fc)
    command('clock_step 333333')
    residual = read(chan+4) & 0xfffc
    assert 508 <= residual <= 512, f'Incorrect DMA residual: {residual}'
    write(chan+12, 1 << 31)
    assert read(chan+4) & (1 << 29), 'Pause did not halt the channel'
    command('clock_step 1000000')
    assert read(chan+4) & 0xfffc == residual, 'Paused residual advanced'
    write(chan+12, 0)
    command('clock_step 334000')
    assert read(chan+4) & (1 << 30), 'Resume restarted the buffer clock'
    # Audio uses PPCS IOVA addresses, including transfers across page edges.
    mc = 0x7000f000
    write(0x90000000, 0xf0090001)
    write(0x90001040, 0xe0090002)
    write(0x90001044, 0xe0090004)
    write(mc+0x1c, 3); write(mc+0x20, 0xe0090000)
    write(mc+0x270, 0x80000003); write(mc+0x10, 1)
    write(bridge+16, 2345)
    write(chan+4, 1 << 30)
    write(chan+16, 0x10ff0)
    write(chan+24, 0x70080010)
    write(chan, 0xc800001c)
    command('clock_step 1000000')
    assert read(0x90004000) >> 16 == 2345, 'PPCS page-crossing DMA used physical IOVA'
    assert read(0x90004004) >> 16 == 2345
    write(chan+4, 1 << 30)
    write(mc+0x10, 0)
    for base in (0x7d000000, 0x7d004000, 0x7d008000):
        assert read(base+0x100) & 255 == 0x30, 'Tegra30 EHCI capability length'
        assert read(base+0x104) & 15 == 1, 'One physical port per controller'
        assert not read(base+0x400) & 128
        write(base+0x400, (1 << 12) | (1 << 11))
        assert not read(base+0x400) & 128, 'UTMI reset must gate PHY clock'
        write(base+0x400, 1 << 12)
        assert read(base+0x400) & 128, 'UTMI enabled but clock unavailable'
        write(base+0x1b4, 1 << 22)
        assert not read(base+0x400) & 128, 'PHY clock-disable ignored'
        write(base+0x1b4, 0)
        assert read(base+0x400) & 128
        write(base+0x400, 0)
        assert not read(base+0x400) & 128
    control, status = 0x7000f200, 0x7000f204
    for mask in (1, 4, 1 << 7, 0x1ffff, 0):
        write(control, mask)
        assert read(status) == mask, 'Flush never completes'
        write(status, ~mask & 0xffffffff)
        assert read(status) == mask, 'Status must be read-only'
    bus = 0x7000c400
    # GPIO DD3: bank 7, port 1, bit 3; the MMX request output.
    write(0x6000d704, 8)
    write(0x6000d714, 8)
    for _ in range(10):
        write(0x6000d724, 0)
        write(0x6000d724, 8)
        assert read(bus+0x70) & 1 << 23
        first = read(bus+0x7c) & 255
        write(bus+0x68, 1 << 23)
        assert read(bus+0x44) & 16
        assert (read(bus+0x80) >> 4) & 0xfff == 23
        frame = bytes([first])+b''.join(read(bus+0x7c).to_bytes(4,'little') for _ in range(6))[:23]
        assert frame[1:3] == b'\x08\x03'
        crc = 0
        for i in (0,1,2,3,4,5,6,11,12,13,14):
            crc ^= frame[i]
            for _ in range(8): crc = ((crc << 1) ^ (0xa6 if crc & 128 else 0)) & 255
        assert crc == frame[15]
        write(bus+0x28, 16)
        assert not read(bus+0x70) & ((1 << 23) | (1 << 25))
    print('PASS: CAR/system timer rates and continuity, TDM DMA data/interrupt/pause/cyclic checks, three USB PHY clock/reset sequences, MC flush, ten IOC receive cycles')
finally:
    p.terminate(); p.wait(timeout=5)

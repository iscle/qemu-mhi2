#!/usr/bin/env python3
"""Exercise packet I2C and SMMU-translated HOST1X syncpoint commands with qtest."""
import argparse
import os
import selectors
import subprocess
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('media', type=Path)
    args = ap.parse_args()
    (args.media / 'tmp').mkdir(exist_ok=True)
    qemu = Path(__file__).resolve().parents[2] / 'build/qemu-system-arm'
    cmd = [str(qemu), '-M', 'mhi2-harman,iram='+str(args.media/'iram.bin'),
           '-bios', str(args.media/'nor.bin'), '-display', 'none', '-serial', 'null',
           '-monitor', 'none', '-S', '-qtest', 'stdio']
    with open('/tmp/mhi2-device-check-host.log', 'w') as err:
        p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=err, bufsize=0,
                             env={**os.environ, "TMPDIR": str(args.media / "tmp")})
        pending = bytearray()
        def q(command):
            p.stdin.write((command+'\n').encode())
            while b'\n' not in pending:
                with selectors.DefaultSelector() as sel:
                    sel.register(p.stdout, selectors.EVENT_READ)
                    assert sel.select(5), command+' timed out'
                data = os.read(p.stdout.fileno(), 65536)
                assert data, 'QEMU exited'
                pending.extend(data)
            line, _, rest = pending.partition(b'\n')
            pending[:] = rest
            assert line.startswith(b'OK'), line
            return line.decode()
        def wr(a, v): q(f'writel {a:#x} {v:#x}')
        def rd(a): return int(q(f'readl {a:#x}').split()[1], 16)
        try:
            assert (rd(0x70000804) >> 8) & 255 == 0x30
            b = 0x7000d000
            wr(b, 0xc00)
            def packet(n, flags, *payload):
                for word in (0x10000010, n-1, flags, *payload): wr(b+0x50, word)
            packet(2, 0x2d << 1, 0xab1b)
            assert rd(b+0x68) & 0x40
            wr(b+0x68, 0xffffffff)
            packet(1, (0x2d << 1) | (1 << 16), 0x1b)
            packet(1, (0x2d << 1) | (1 << 19))
            assert rd(b+0x60) & 15 == 1
            assert rd(b+0x54) & 255 == 0xab
            assert rd(b+0x60) & 15 == 0
            packet(1, (0x7e << 1) | (1 << 19))
            assert rd(b+0x68) & 8, 'missing slave must NACK'
            # ASID 3: IOVA 0x10000 -> PA 0x90002000 via two-level page tables.
            wr(0x90000000, 0xf0090001)
            wr(0x90001040, 0xe0090002)
            wr(0x90002000, 0x40)       # SETCLASS host1x (class 1)
            wr(0x90002004, 0x40000001) # IMM increment syncpoint 1
            mc = 0x7000f000
            wr(mc+0x1c, 3); wr(mc+0x20, 0xe0090000)
            wr(mc+0x250, 0x80000003); wr(mc+0x10, 1)
            h = 0x50000000
            wr(h+0x24, 1); wr(h+0x18, 0x10000); wr(h+0x24, 7)
            wr(h+0x24, 0); wr(h+0x3504, 1); wr(h+0x3068, 2)
            assert rd(h+0x3040) & 2 == 0
            wr(h+0x18, 0x10008)
            assert rd(h+0x1c) == 0x10008
            assert rd(h+0x3404) == 1
            assert rd(h+0x3040) & 2
            wr(h+0x3060, 2); wr(h+0x3040, 2)
            assert rd(h+0x3040) & 2 == 0
            # Tegra30 channels are 16 KiB apart; channel 2 is used by QNX GR2D.
            ch2 = h + 0x8000
            wr(ch2+0x24, 1); wr(ch2+0x18, 0x10000); wr(ch2+0x24, 7)
            wr(ch2+0x24, 0); wr(ch2+0x18, 0x10008)
            assert rd(ch2+0x1c) == 0x10008
            assert rd(h+0x3404) == 2
            # Gather actual display-class methods through the same IOVA mapping.
            words = [0x00001c00, 0xe0000006, 0x20080001, 0x00000303,
                     0x00000040, 0x20080001, 0x03000001, 0xe1000006]
            for i, word in enumerate(words): wr(0x90002100+i*4, word)
            wr(0x90002008, 0x60000008); wr(0x9000200c, 0x10100)
            wr(h+0x18, 0x10010)
            assert rd(h+0x1c) == 0x10010
            assert rd(h+0x340c) == 1
            assert rd(h+0x3358) == 0, 'display mlock must be released'
            # Identity RGB565 stretch blit, distinct source/destination pixels.
            wr(mc+0x24c, 0x80000003)
            wr(0x90002200, 0x1234abcd); wr(0x90002300, 0)
            regs = {9: 0x38, 0xc: 1, 0x1c: 0x0808, 0x1d: 0x700000,
                    0x2b: 0x10300, 0x31: 0x10200, 0x2e: 16, 0x33: 16,
                    0x11: 0x1000, 0x13: 0x1000, 0x37: 2, 0x38: 2}
            words = [0x52 << 6]
            for reg, value in regs.items(): words += [0x20000001 | (reg << 16), value]
            words += [0x40000112]  # OP_DONE increments GR2D syncpoint 18
            for i, word in enumerate(words): wr(0x90002400+i*4, word)
            wr(ch2+0x24, 1); wr(ch2+0x18, 0x10400); wr(ch2+0x24, 7)
            wr(ch2+0x24, 0); wr(ch2+0x18, 0x10400+len(words)*4)
            assert rd(0x90002300) == 0x1234abcd, 'GR2D did not copy pixels'
            assert rd(h+0x3448) == 1, 'GR2D completion missing'
            # Production PCI probe: root port 1, TI RCC endpoint, BAR sizing.
            assert rd(0x1000) == 0x0e1c10de
            assert rd(0x01010000) == 0xffffffff, 'unassigned bus must not enumerate'
            wr(0x1018, 0x00010100)
            cfg = 0x01010000
            assert rd(cfg) == 0xb800104c
            assert rd(cfg+0x800) == 0xffffffff, 'absent slot must not enumerate'
            wr(cfg+0x10, 0xffffffff); wr(cfg+0x14, 0xffffffff)
            assert rd(cfg+0x10) == 0xfffff000
            assert rd(cfg+0x14) == 0xffffc000
            wr(cfg+0x10, 0x20000000); wr(cfg+0x14, 0x20004000)
            assert rd(0x20004000) == 0
            wr(0x2000007c, 0xdeadbeef)
            assert rd(0x20004000) == 1
            assert rd(0x20004018) == 0x48
            assert rd(0x2000401c) == 32
            assert rd(0x20004030) == 0x3c8
            assert rd(0x20004034) == 32
            assert rd(0x20004044) == 31
            # Native MOST ISO channel 3: BAR3 mailbox -> bounded DMA -> MSI 5.
            wr(cfg+0x1c,0xffffffff)
            assert rd(cfg+0x1c)==0xfffff000
            wr(cfg+0x1c,0x20008000)
            wr(0x20000200,0x90000001)
            wr(0x90006000,0x22222222)
            wr(0x2000800c,0x22222222)
            wr(0x2000802c,0x20006000)
            wr(0x2000804c,188)
            wr(0x20000054,5)
            assert rd(0x90006000)==0, 'MOST transmit completion missing'
            assert rd(0x386c)&(1<<5), 'MOST channel MSI missing'
            wr(0x90006000,0x12345678)
            wr(0x2000800c,0x22222222)
            wr(0x2000804c,0x100001)
            wr(0x20000054,5)
            assert rd(0x90006000)==0x12345678, 'oversized MOST DMA accepted'
            print('PASS: Tegra ID, I2C, SMMU DMA, HOST1X, GR2D pixel copy, PCIe enumeration/BAR sizing and RCC ring publication')
        finally:
            p.terminate()
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill(); p.wait()


if __name__ == '__main__':
    main()

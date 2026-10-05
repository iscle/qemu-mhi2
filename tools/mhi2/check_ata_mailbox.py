#!/usr/bin/env python3
"""Exercise the native RCC ATA mailbox against QEMU's optical device core."""
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile

media = Path(sys.argv[1]).resolve()
qemu = Path(__file__).resolve().parents[2] / 'build/qemu-system-arm'
with tempfile.TemporaryDirectory(prefix='mhi2-ata-') as tmp:
    disc = Path(tmp) / 'disc.raw'
    contents = bytes((i * 17 + i // 2048) & 255 for i in range(2048 * 64))
    disc.write_bytes(contents)
    for inserted in (False, True):
        args = [str(qemu), '-M', 'mhi2-harman,iram='+str(media/'iram.bin'),
                '-bios', str(media/'nor.bin'), '-display', 'none', '-serial', 'null',
                '-monitor', 'none', '-accel', 'qtest', '-qtest', 'stdio']
        if inserted:
            args += ['-drive', f'if=none,id=dvd,file={disc},format=raw,readonly=on',
                     '-global', 'tegra30-pcie.dvd-drive=dvd']
        with open('/tmp/mhi2-ata-test.log', 'w') as log:
            p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=log, bufsize=0)
            pending = bytearray()
            def q(command):
                p.stdin.write((command+'\n').encode())
                while b'\n' not in pending:
                    assert select.select([p.stdout], [], [], 5)[0], command
                    block = p.stdout.read(4096)
                    assert block, Path('/tmp/mhi2-ata-test.log').read_text()[-2000:]
                    pending.extend(block)
                line, _, rest = pending.partition(b'\n'); pending[:] = rest
                assert line.startswith(b'OK'), line
                return line.split()[1:]
            def rd(a): return int(q(f'readl {a:#x}')[0], 16)
            def wr(a, v): q(f'writel {a:#x} {v:#x}')
            def put(a, b): q(f'write {a:#x} {len(b)} 0x{b.hex()}')
            def get(a, n): return bytes.fromhex(q(f'read {a:#x} {n}')[0].decode()[2:])
            cfg, app, box, ram = 0x01010000, 0x20000000, 0x20008000, 0x81000000
            def submit(op, count, payload, extra=0, valid=True):
                wr(box+8, op | 128); wr(box+12, count); wr(box+16, extra)
                put(box+20, payload)
                wr(app+0x54, 1)
                assert bool(rd(box+8) & 128) != valid, (op, count, payload.hex())
            def regs(items):
                submit(1, len(items), b''.join(struct.pack('<HBBI', *i) for i in items))
                return [rd(box+24+8*i) for i in range(len(items))]
            def port(a, v=None, bits=8):
                return regs([(a, bits, int(v is None), v or 0)])[0]
            def block(data=None, length=0, bits=16):
                length = len(data) if data is not None else length
                submit(2, length//(bits//8), struct.pack('<HBB', 0x1f0, bits,
                       int(data is None)) + (data or bytes(length)))
                return get(box+24, length)
            def packet(cdb, dma=False):
                regs([(0x1f1,8,0,int(dma)), (0x1f4,8,0,0), (0x1f5,8,0,0x20),
                      (0x1f7,8,0,0xa0)])
                assert port(0x1f7) & 8
                block(cdb.ljust(12,b'\0'))
            def pio_reply():
                out=bytearray()
                for _ in range(100):
                    status=port(0x1f7)
                    assert not status & 1, hex(status)
                    if not status & 8: return bytes(out)
                    length=port(0x1f4) | port(0x1f5)<<8
                    out.extend(block(length=length))
                raise AssertionError('PIO reply did not terminate')
            def prepare_dma(descriptors):
                payload=b''.join(struct.pack('<II', addr,
                    length | (0x80000000 if i==len(descriptors)-1 else 0))
                    for i,(addr,length) in enumerate(descriptors))
                submit(3,1,payload,extra=1)
            def dma_finish():
                for _ in range(1000):
                    q('clock_step 1000000')
                    status=port(2,bits=16)
                    if not status & 1: break
                assert status & 4 and not status & 3, hex(status)
                assert not port(0x1f7) & 0x89
                submit(4,1,b'\0'*4)
                assert port(2,bits=16)==0 and port(0,bits=16)==0
            try:
                wr(0x1018,0x100)
                wr(cfg+0x18,0xffffffff); assert rd(cfg+0x18)==0xffffc000
                wr(cfg+0x10,app); wr(cfg+0x18,box)
                assert rd(box)==0x80000001 and rd(box+4)==100000000
                wr(box,0); assert rd(box)==0x80000001
                wr(cfg+0x50,0x410005); wr(cfg+0x58,0x20)
                wr(0x3890,2); wr(0x38b4,0x100)
                port(0x3f6,4); port(0x3f6,0)
                assert port(0x1f4)==0x14 and port(0x1f5)==0xeb
                port(0x1f7,0xa1)
                assert rd(0x3870) & 2, 'ATA MSI vector missing'
                wr(0x3870,2); assert rd(0x3870)==0
                identify=block(length=512,bits=32)
                words=struct.unpack('<256H',identify)
                assert words[0] & 0x8000 and words[49] & 0x100
                assert not port(0x1f7) & 8
                packet(bytes([0x12,0,0,0,36]))
                inquiry=pio_reply()
                assert len(inquiry)==36 and inquiry[0]==5 and inquiry[1]&0x80
                packet(bytes([0]))
                if not inserted:
                    assert port(0x1f7)&1
                    packet(bytes([3,0,0,0,18]))
                    sense=pio_reply()
                    assert sense[2]&15==2 and sense[12]==0x3a, sense.hex()
                else:
                    assert not port(0x1f7)&1
                # Timing state and invalid lists: validate atomically.
                port(0x50,0x201,bits=32)
                submit(1,2,struct.pack('<HBBI',0x50,32,0,0xdead)+
                       struct.pack('<HBBI',0xffff,8,0,1),valid=False)
                assert port(0x50,bits=32)==0x201
                submit(2,0xffffffff,struct.pack('<HBB',0x1f0,32,1),valid=False)
                submit(2,1,struct.pack('<HBB',0x1f0,0,1),valid=False)
                submit(3,1,struct.pack('<II',0x20000000,0x80040002),extra=1,valid=False)
                submit(4,1,bytes(4))
                # A fragmented DMA inquiry also tests command-before-start.
                wr(app+0x200,ram|1)
                put(ram,bytes(256)); prepare_dma([(0x20000000,16),(0x20000080,20)])
                packet(bytes([0x12,0,0,0,36]),dma=True)
                assert get(ram,16)==bytes(16), 'DMA ran before START'
                port(0,9,bits=16); dma_finish()
                assert get(ram,16)+get(ram+128,20)==inquiry
                if inserted:
                    # READ(10) exercises asynchronous block I/O and the
                    # native driver's 32-bit descriptor length (>64 KiB).
                    length=40*2048
                    prepare_dma([(0x20001000,length)])
                    port(0,9,bits=16)  # START before command also works
                    packet(bytes([0x28,0,0,0,0,2,0,0,40,0]),dma=True)
                    dma_finish()
                    assert get(ram+4096,length)==contents[4096:4096+length]
                # Bad outbound mapping fails the transfer, not host memory.
                wr(app+0x200,0); prepare_dma([(0x20000000,36)])
                packet(bytes([0x12,0,0,0,36]),dma=True); port(0,9,bits=16)
                assert port(2,bits=16)&6==6 and port(0x1f7)&1
                submit(4,1,bytes(4))
                print(f'PASS: ATA mailbox, PIO, MSI, bounded DMA, empty tray / media={inserted}')
            finally:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired: p.kill(); p.wait()

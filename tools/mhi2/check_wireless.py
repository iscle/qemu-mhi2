#!/usr/bin/env python3
"""Exercise the Marvell SDIO radio through SDHCI, Ethernet and standard H4 sockets."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time
from wireless import arguments as wireless_arguments


class Radio:
    def __init__(self, qemu, media, root, bluetooth=True, user=False, explicit=False, disabled=False):
        self.eth, eth_guest = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.bt, bt_guest = socket.socketpair()
        self.eth.settimeout(3)
        self.bt.settimeout(3)
        self.log = (root/'host.log').open('w')
        qmp_path = str(root/'qmp')
        wifi = 'none' if disabled or explicit else (
            ('user' if user else 'socket,fd='+str(eth_guest.fileno()))+
            ',mac=02:00:02:87:87:01,id=wifi')
        self.ssid = 'QEMU, réseau' if explicit else 'QEMU Wi-Fi'
        args = [str(qemu), '-M', 'mhi2-harman,iram='+str(media/'iram.bin'),
                '-bios', str(media/'nor.bin'), '-display', 'none', '-serial', 'null',
                '-monitor', 'none', '-accel', 'qtest', '-qtest', 'stdio',
                '-qmp', 'unix:'+qmp_path+',server=on,wait=off',
                *wireless_arguments(wifi, self.ssid)]
        if explicit:
            args += ['-netdev', 'user,id=wifi', '-global', 'mv8787-sdio.netdev=wifi',
                     '-global', 'mv8787-sdio.mac=02:00:02:87:87:01']
        if bluetooth:
            args += ['-chardev', f'socket,id=bt,fd={bt_guest.fileno()}',
                     '-global', 'mv8787-sdio.bluetooth-chardev=bt']
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.log, bufsize=0,
                                     pass_fds=(eth_guest.fileno(), bt_guest.fileno()))
        eth_guest.close(); bt_guest.close()
        self.pending = bytearray()
        self.qmp_socket = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.qmp_socket.connect(qmp_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if self.proc.poll() is not None:
                    raise RuntimeError((root/'host.log').read_text())
                time.sleep(.02)
        self.qmp = self.qmp_socket.makefile('rwb', buffering=0)
        self.qmp.readline()
        self.monitor('qmp_capabilities')
        self.wr(0x28, 0xf00); self.wr(0x2c, 7); self.wr(0x34, 0xffffffff)
        assert self.cmd(5, 0) == 0xb0ff8000
        self.cmd(3, 0); self.cmd(7, 1 << 16)
        self.reg(0, 2, 0x0e)
        for f in (1, 2, 3):
            self.reg(0, f*256+0x10, 64); self.reg(0, f*256+0x11, 0)
            self.reg(f, 2, 3)
        self.reg(0, 4, 0x0f)
        # A valid zero-payload terminal download record exercises the ROM path.
        self.tx(1, struct.pack('<4I', 4, 0, 0, 0))
        assert self.reg(1, 0x60) | self.reg(1, 0x61) << 8 == 0xfedc

    def monitor(self, command, arguments=None):
        self.qmp.write((json.dumps(dict(execute=command, arguments=arguments or {}))+'\n').encode())
        while True:
            item = json.loads(self.qmp.readline())
            assert 'error' not in item, item
            if 'return' in item: return item['return']

    def q(self, command):
        self.proc.stdin.write((command+'\n').encode())
        while b'\n' not in self.pending:
            assert select.select([self.proc.stdout], [], [], 5)[0], command
            data = os.read(self.proc.stdout.fileno(), 65536)
            assert data, command
            self.pending.extend(data)
        line, _, self.pending = self.pending.partition(b'\n')
        assert line.startswith(b'OK'), line
        return line.split()[1:]

    def rd(self, offset): return int(self.q(f'readl {0x78000200+offset:#x}')[0], 16)
    def wr(self, offset, value): self.q(f'writel {0x78000200+offset:#x} {value:#x}')

    def cmd(self, command, arg, length=0, read=False):
        self.wr(0x30, 0xffffffff)
        if length: self.wr(4, (length//64 << 16) | 64)
        self.wr(8, arg)
        self.wr(0xc, ((command << 8) | 2 | (0x20 if length else 0)) << 16 |
                (0x12 if read else 2) | (0x20 if length > 64 else 0))
        assert self.rd(0x30) & 0x8001 == 1, command
        return self.rd(0x10)

    def reg(self, function, address, value=None):
        return self.cmd(52, function << 28 | address << 9 |
                        ((1 << 31) | value if value is not None else 0)) & 255

    def tx(self, function, data):
        data = data.ljust((len(data)+63)//64*64, b'\0')
        arg = 1 << 31 | function << 28 | 1 << 27 | 0x10000 << 9 | len(data)//64
        assert not self.cmd(53, arg, len(data)) & 0x300
        for value, in struct.iter_unpack('<I', data): self.wr(0x20, value)
        assert self.rd(0x30) & 2

    def rx(self, function):
        deadline = time.monotonic()+3
        while not self.reg(function, 0x30) & 2:
            assert time.monotonic() < deadline, f'RX timeout function {function}'
            time.sleep(.001)
        bitmap = self.reg(function, 4) | self.reg(function, 5) << 8
        port = (bitmap & -bitmap).bit_length() - 1 if function == 1 else 0
        length = self.reg(function, 8 + 2*port) | self.reg(function, 9 + 2*port) << 8
        units = self.reg(function, 0x62) << self.reg(function, 0x63)
        assert units >= length
        rounded = (length+63)//64*64
        self.cmd(53, function << 28 | 1 << 27 | (0x10000 + port) << 9 | rounded//64, rounded, True)
        packet = b''.join(struct.pack('<I', self.rd(0x20)) for _ in range(rounded//4))
        return packet[:length]

    def wlan(self, op, payload=b'', seq=1, wait=True):
        self.tx(1, struct.pack('<6H', 12+len(payload), 1, op, 8+len(payload), seq, 0)+payload)
        if not wait: return
        packet = self.rx(1)
        assert struct.unpack_from('<H', packet, 4)[0] == op | 0x8000, packet.hex()
        return struct.unpack_from('<H', packet, 10)[0], packet[12:]

    def hci(self, packet):
        self.tx(2, struct.pack('<I', len(packet)-1)[:3]+packet)

    def close(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)
        self.log.close(); self.eth.close(); self.bt.close()
        self.qmp.close(); self.qmp_socket.close()


def associate(radio):
    bssid = bytes.fromhex('525400878701')
    assert radio.wlan(0xa9)[0] == 0
    status, scan = radio.wlan(6, b'\x03'+bytes(6))
    ssid = radio.ssid.encode()
    assert status == 0 and scan[2] == 1 and ssid in scan
    assert scan[5:11] == bssid
    status, scan = radio.wlan(6, b'\x03'+bytes(6)+struct.pack('<HH', 0, 7)+b'foreign')
    assert status == 0 and scan[2] == 0
    assert radio.wlan(6, bytes(6))[0] == 2
    payload = struct.pack('<6sHHHB', bssid, 0x21, 10, 100, 1)+struct.pack('<HH', 0, len(ssid))+ssid
    result, body = radio.wlan(0x12, payload)
    assert result == 0 and struct.unpack_from('<H', body, 2)[0] == 0, body


def ethernet_tx(radio, frame):
    radio.tx(1, ethernet_packet(frame))


def ethernet_packet(frame):
    pd = struct.pack('<BBHHHIBBBB', 0, 0, len(frame), 16, 0, 0, 0, 0, 0, 0)
    return struct.pack('<HH', len(pd)+len(frame)+4, 0)+pd+frame


def ethernet_rx(radio):
    packet = radio.rx(1)
    assert packet[2:4] == b'\0\0', packet.hex()
    length, offset = struct.unpack_from('<HH', packet, 6)
    assert offset >= 20 and length >= 14
    return packet[4+offset:4+offset+length]


def test_radio(r):
    associate(r)
    frame = bytes.fromhex('ffffffffffff0200028787010800')+bytes(range(240))*4
    ethernet_tx(r, frame)
    assert r.eth.recv(4096) == frame
    r.eth.send(frame)
    assert ethernet_rx(r) == frame
    # Native drivers rotate the data port through 1..15 (port 0 is commands).
    for i in range(32):
        packet = frame + bytes([i])
        r.eth.send(packet)
        assert ethernet_rx(r) == packet
    # Several Ethernet frames may share one block-padded SDIO transaction.
    batch = [frame + bytes([i]) for i in range(4)]
    r.tx(1, b''.join(p.ljust((len(p)+63)//64*64, b'\0')
                    for p in map(ethernet_packet, batch)))
    for packet in batch: assert r.eth.recv(4096) == packet
    # Simultaneous events and command completions must retain FIFO ordering.
    for i in range(8): r.wlan(0x4d, bytes(8), seq=i, wait=False)
    for i in range(8):
        response = r.rx(1)
        assert struct.unpack_from('<H', response, 8)[0] == i
    r.hci(bytes.fromhex('01030c00'))
    assert r.bt.recv(4) == bytes.fromhex('01030c00')
    event = bytes.fromhex('040e0401030c00')
    for byte in event: r.bt.send(bytes([byte]))
    assert r.rx(2)[3:] == event
    # Discard an oversized ACL frame without losing the next H4 boundary.
    r.bt.sendall(b'\x02'+struct.pack('<HH', 1, 4100)+bytes(4100)+event)
    assert r.rx(2)[3:] == event
    # Full-size ACL, SCO, and coalesced asynchronous HCI events.
    acl = b'\x02'+struct.pack('<HH', 0x2001, 1021)+bytes(range(256))*3+bytes(253)
    r.bt.sendall(event+acl+event)
    assert r.rx(2)[3:] == event
    assert r.rx(2)[3:] == acl
    assert r.rx(2)[3:] == event
    r.hci(acl)
    got = b''
    while len(got) < len(acl): got += r.bt.recv(len(acl)-len(got))
    assert got == acl
    sco = bytes.fromhex('03010003aabbcc')
    r.hci(sco); assert r.bt.recv(len(sco)) == sco
    r.bt.sendall(sco); assert r.rx(2)[3:] == sco
    r.monitor('set_link', dict(name='wifi', up=False))
    assert struct.unpack_from('<I', r.rx(1), 4)[0] & 0xffff == 8
    assert r.wlan(6, b'\x03'+bytes(6))[1][2] == 0
    r.bt.close()
    assert r.rx(2)[3:] == bytes.fromhex('04100101')
    print('PASS: SDIO scan/association, Ethernet both directions, queued commands, fragmented H4, full ACL/SCO, link/backend loss')


def test_user(r):
    associate(r)
    # ARP resolves the real libslirp gateway across the emulated WLAN.
    mac = bytes.fromhex('020002878701')
    arp = struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1)+mac+socket.inet_aton('10.0.2.15')+bytes(6)+socket.inet_aton('10.0.2.2')
    ethernet_tx(r, b'\xff'*6+mac+b'\x08\x06'+arp)
    reply = ethernet_rx(r)
    assert reply[12:14] == b'\x08\x06' and reply[20:22] == b'\x00\x02'
    assert reply[28:32] == socket.inet_aton('10.0.2.2')
    r.hci(bytes.fromhex('01010405338b9e0100'))
    assert r.rx(2)[3:] == bytes.fromhex('040f0400010104')
    assert r.rx(2)[3:] == bytes.fromhex('04010100')
    print('PASS: real QEMU user-network gateway via WLAN, built-in Bluetooth empty inquiry')


async def test_bumble(r):
    """Use an independent software HCI controller, not canned test replies."""
    from bumble.controller import Controller
    from bumble.transport.common import StreamPacketSource
    class Sink:
        def on_packet(self, packet):
            r.bt.sendall(packet)
    source = StreamPacketSource()
    Controller('mhi2-test', source, Sink(), public_address='F0:F1:F2:F3:F4:F5')
    async def serve():
        while True:
            data = await asyncio.to_thread(r.bt.recv, 4096)
            if not data: return
            source.data_received(data)
    server = asyncio.create_task(serve())
    try:
        for command in ('01030c00', '01011000', '01091000'):
            await asyncio.to_thread(r.hci, bytes.fromhex(command))
            response = (await asyncio.to_thread(r.rx, 2))[3:]
            assert response[0:2] == b'\x04\x0e' and response[4:6] == bytes.fromhex(command)[1:3]
            assert response[6] == 0
        assert response[7:] == bytes.fromhex('f5f4f3f2f1f0')
        print('PASS: independent Bumble controller reset, version and Bluetooth address through SDIO/H4')
    finally:
        r.bt.shutdown(socket.SHUT_RDWR)
        await server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--media', type=Path, help='Optional prepared NOR/IRAM; defaults to blank test images')
    parser.add_argument('--bumble', action='store_true', help='Also test the optional Bumble controller')
    args = parser.parse_args()
    for mode in ['socket', 'user', 'netdev', 'disabled'] + (['bumble'] if args.bumble else []):
        with tempfile.TemporaryDirectory(prefix='mhi2-wireless-') as temp:
            root = Path(temp)
            media = args.media.resolve() if args.media else root
            if not args.media:
                for name, size in [('nor.bin', 64*1024*1024), ('iram.bin', 256*1024)]:
                    with (media/name).open('wb') as output: output.truncate(size)
            radio = Radio(args.qemu.resolve(), media, root,
                          bluetooth=mode in ('socket', 'bumble'), user=mode == 'user',
                          explicit=mode == 'netdev', disabled=mode == 'disabled')
            try:
                if mode == 'bumble': asyncio.run(test_bumble(radio))
                elif mode == 'disabled':
                    assert radio.wlan(0xa9)[0] == 0
                    assert radio.wlan(6, b'\x03'+bytes(6))[1][2] == 0
                    print('PASS: disabled network has no discoverable AP')
                else: (test_user if mode in ('user', 'netdev') else test_radio)(radio)
            finally: radio.close()


if __name__ == '__main__': main()

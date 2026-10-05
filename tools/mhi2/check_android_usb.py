#!/usr/bin/env python3
"""Exercise accessory enumeration and real EHCI DMA against a loopback peer.

Uses qtest, not the firmware or Android Auto. No projection protocol is mocked.
"""
import os, select, socket, struct, subprocess, sys, time
from pathlib import Path
media=Path(sys.argv[1]).resolve()
with_hub = '--hub' in sys.argv[2:]
qemu=Path(__file__).resolve().parents[2]/'build/qemu-system-arm'
server=socket.socket();server.bind(('127.0.0.1',0));server.listen(1);server.settimeout(5)
port=server.getsockname()[1]
devices = (['-device', 'usb-mhi2-hub,bus=usb-bus.0,port=1,ports=3,port-power=on',
            '-device', 'usb-mhi2-hfc,bus=usb-bus.0,port=1.3'] if with_hub else [])
phone_port = '1.1' if with_hub else '1'
log=(media.parent/'logs/usb-qtest.log').open('w')
p=subprocess.Popen([str(qemu),'-M',f'mhi2-harman,iram={media}/iram.bin','-bios',str(media/'nor.bin'),
    '-display','none','-serial','null','-monitor','none','-accel','qtest','-qtest','stdio',
    '-chardev',f'socket,id=aa,host=127.0.0.1,port={port},reconnect-ms=100',
    *devices, '-device',f'usb-android-auto,chardev=aa,bus=usb-bus.0,port={phone_port}'],
    stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,bufsize=0)
pending=bytearray();peer=None

def cmd(text):
    p.stdin.write((text+'\n').encode())
    while b'\n' not in pending:
        assert select.select([p.stdout],[],[],5)[0],text
        b=os.read(p.stdout.fileno(),65536);assert b,('QEMU exited',p.poll());pending.extend(b)
    line,_,rest=pending.partition(b'\n');pending[:]=rest
    assert line.startswith(b'OK'),line
    return line.split()[1:]
def rd(a):return int(cmd(f'readl {a:#x}')[0],16)
def wr(a,v):cmd(f'writel {a:#x} {v:#x}')
def put(a,data):cmd(f'write {a:#x} {len(data)} 0x{data.hex()}')
def get(a,n):return bytes.fromhex(cmd(f'read {a:#x} {n}')[0].decode()[2:]) if n else b''
def words(a,values):put(a,struct.pack('<'+'I'*len(values),*values))
def step():cmd('clock_step 1000000')
base=0x7d000000;op=base+0x130;qh=0x81000000;td=qh+0x100;buf=qh+0x1000
address=0

def transfer(stages,endpoint=0):
    # Each stage: PID (OUT=0, IN=1, SETUP=2), byte count, data toggle, bytes.
    wr(op,1);step()
    for i,(pid,size,toggle,data) in enumerate(stages):
        dest=buf+i*0x5000
        if data:put(dest,data)
        token=(toggle<<31)|(size<<16)|(1<<15)|(3<<10)|(pid<<8)|128
        words(td+i*0x40,[td+(i+1)*0x40 if i+1<len(stages) else 1,1,token,
              dest,*[dest+j*4096 for j in range(1,5)],0,0,0,0,0])
    words(qh,[qh|2,address|(endpoint<<8)|(2<<12)|(1<<14)|(1<<15)|((64 if endpoint==0 else 512)<<16),
              1<<30,0,td,1,0,0,0,0,0,0,0,0,0,0,0])
    wr(op+0x18,qh);wr(op,0x21)
    for _ in range(250):
        step()
        tokens=[rd(td+i*0x40+8) for i in range(len(stages))]
        if any(t&0x7c for t in tokens):raise AssertionError(('USB transfer error',[hex(t) for t in tokens]))
        if not any(t&128 for t in tokens):break
    else:raise AssertionError(('Transfer did not complete',tokens,hex(rd(op+0x44))))
    result=[get(buf+i*0x5000,size-((tokens[i]>>16)&0x7fff)) if pid==1 else b''
            for i,(pid,size,_,_) in enumerate(stages)]
    wr(op,1);step()
    return result

def control(request,value=0,index=0,size=0,kind=0x80,data=b''):
    stages=[(2,8,0,struct.pack('<BBHHH',kind,request,value,index,size))]
    if size:stages.append((1 if kind&128 else 0,size,1,data))
    stages.append((0 if kind&128 else 1,0,1,b''))
    result=transfer(stages)
    return result[1] if size and kind&128 else b''

try:
    peer,_=server.accept();peer.settimeout(5)
    wr(base+0x400,1<<12)
    wr(op,2);step();wr(op+0x40,1)
    wr(op+0x44,(1<<12)|(1<<8));cmd('clock_step 60000000');wr(op+0x44,1<<12);step()
    assert rd(op+0x44)&5==5,hex(rd(op+0x44))
    assert (rd(base+0x1b4)>>25)&3==2, 'HOSTPC must report high speed'
    wr(base+0x1b4,3<<25)
    assert (rd(base+0x1b4)>>25)&3==2, 'HOSTPC speed must be read-only'
    if with_hub:
        descriptor = control(6, 0x100, size=18)
        assert descriptor[8:12] == bytes.fromhex('24043225'), descriptor.hex()
        control(5, 1, kind=0); address = 1
        control(9, 1, kind=0)
        hub = control(6, 0x2900, size=9, kind=0xa0)
        assert hub[2] == 3, hub.hex()
        assert hub[7] == 8, 'Only the internal HFC port is non-removable'
        control(3, 8, 3, kind=0x23)  # HFC port power, then reset.
        control(3, 4, 3, kind=0x23)
        assert control(0, index=3, size=4, kind=0xa3)[1] & 4
        address = 0
        descriptor = control(6, 0x100, size=18)
        assert descriptor[8:12] == bytes.fromhex('24043025'), descriptor.hex()
        control(5, 2, kind=0); address = 2
        control(9, 1, kind=0)
        capability = control(6, 0x300 | descriptor[15], 0x409, 255)
        assert capability[2:].decode('utf-16le') == 'Ve10Di55P1u1abP2u2abP3n'
        control(3, 0x837, size=3, kind=0x41, data=b'\x01\x23\x45')
        assert control(4, 0x837, size=3, kind=0xc1) == b'\x01\x23\x45'
        control(0x70, kind=0x41)
        address = 1
        control(3, 8, 1, kind=0x23)
        control(3, 4, 1, kind=0x23)
        address = 0
    descriptor=control(6,0x100,size=18)
    assert descriptor[8:12]==bytes.fromhex('d118002d'),descriptor.hex()
    phone_address = 3 if with_hub else 1
    control(5,phone_address,kind=0);address=phone_address
    config=control(6,0x200,size=32)
    assert config[18:25]==bytes.fromhex('07058102000200'),config.hex()
    control(9,1,kind=0)
    assert control(51,size=2,kind=0xc0)==b'\x02\0'
    for size in (1,511,512,513,8192):
        payload=bytes((i*13+size)&255 for i in range(size))
        transfer([(0,size,0,payload)],2)
        actual=bytearray()
        while len(actual)<size:actual.extend(peer.recv(size-len(actual)))
        assert actual==payload,(size,len(actual))
        peer.sendall(payload)
        # Process the host chardev input before scheduling the USB IN request.
        for _ in range(4):step()
        result=bytearray()
        while len(result)<size:
            result.extend(transfer([(1,min(8192,size-len(result)),0,b'')],1)[0])
        assert result==payload,(size,len(result))
    peer.close();peer=None
    for _ in range(10):step()
    if with_hub:
        address = 1
        assert not control(0, index=1, size=4, kind=0xa3)[0] & 1
        assert rd(op+0x44)&1, 'Phone removal disconnected the hub'
    else:
        assert not rd(op+0x44)&1,'Disconnect did not remove USB accessory'
        assert rd(base+0x1b4)&(3<<25)==0, 'Disconnected HOSTPC retains stale speed'
    print('PASS: Tegra EHCI enumeration, AOA protocol query, bidirectional bulk 1/511/512/513/8192 bytes and disconnect')
finally:
    if peer:peer.close()
    server.close();p.terminate();p.wait(timeout=5);log.close()

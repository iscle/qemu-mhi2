#!/usr/bin/env python3
"""Check the cellular modem via USB EHCI DMA, AT commands and real NAT Ethernet."""
import argparse
import os
from pathlib import Path
import select
import socket
import struct
import tempfile
import time
from check_wireless import Radio
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qemu',type=Path,required=True)
parser.add_argument('--mode', choices=('3g', '4g'), default='4g')
args=parser.parse_args()
temporary=tempfile.TemporaryDirectory(prefix='mhi2-modem-')
root=Path(temporary.name)
for name,size in [('nor.bin',64*1024*1024),('iram.bin',256*1024)]:
    with (root/name).open('wb') as f:f.truncate(size)
r=Radio(args.qemu.resolve(),root,root,bluetooth=False,disabled=True,
        extra_args=['-netdev','user,id=cellular,net=10.0.3.0/24',
                    '-device','usb-mhi2-modem,id=modem,bus=usb-bus.2,port=1,netdev=cellular,mac=02:00:02:87:87:02,lte='+('on' if args.mode=='4g' else 'off')])
p=r.proc
pending=bytearray()
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
base=0x7d008000;op=base+0x130;qh=0x81000000;td=qh+0x100;buf=qh+0x1000
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

def at(text):
    data=(text+'\r').encode()
    for offset in range(0,len(data),512):
        part=data[offset:offset+512]
        transfer([(0,len(part),0,part)],3)
    return transfer([(1,512,0,b'')],3)[0]

def send_frame(frame):
    for offset in range(0,len(frame),512):
        part=frame[offset:offset+512]
        transfer([(0,len(part),0,part)],6)
    if len(frame)%512==0:
        transfer([(0,0,0,b'')],6)

def recv_frame():
    frame=bytearray()
    while True:
        part=transfer([(1,512,0,b'')],6)[0]
        frame.extend(part)
        if len(part)<512:return bytes(frame)

def checksum(data):
    data+=bytes(len(data)%2)
    total=sum(struct.unpack('!'+str(len(data)//2)+'H',data))
    while total>>16:total=(total&65535)+(total>>16)
    return ~total&65535

def udp_frame(mac,dest_mac,source,dest,sport,dport,payload):
    udp=struct.pack('!HHHH',sport,dport,8+len(payload),0)+payload
    ip=bytearray(struct.pack('!BBHHHBBH4s4s',0x45,0,20+len(udp),1,0,64,17,0,
                            socket.inet_aton(source),socket.inet_aton(dest)))
    struct.pack_into('!H',ip,10,checksum(ip))
    return dest_mac+mac+b'\x08\x00'+ip+udp
try:
    wr(base+0x400,1<<12)
    wr(op,2);step();wr(op+0x40,1)
    wr(op+0x44,(1<<12)|(1<<8));cmd('clock_step 60000000');wr(op+0x44,1<<12);step()
    descriptor=control(6,0x100,size=18)
    assert descriptor[8:12]==bytes.fromhex('2d1e6000'),descriptor.hex()
    control(5,1,kind=0);address=1
    config=control(6,0x200,size=1024)
    assert config[4]==6
    control(9,1,kind=0)
    assert b'OK' in at('ATE0')
    assert b'+CPIN: READY' in at('AT+CPIN?')
    assert b'001010123456789' in at('AT+CIMI')
    assert ('"00101",'+('7' if args.mode=='4g' else '2')).encode() in at('AT+COPS?')
    assert b'ERROR' in at('AT+THISDOESNOTEXIST')
    assert b'ERROR' in at('AT+CFUN=1garbage')
    assert b'ERROR' in at('AT+CGDCONT=')
    assert b'ERROR' in at('AT'+('X'*1100)+';^SWWAN=1,1')
    assert b'^SWWAN: 1,0,1' in at('AT^SWWAN?')
    assert b'OK' in at('AT+CGDCONT=1,"IP","qemu;Test"')
    assert b'"qemu;Test"' in at('AT+CGDCONT?')
    assert b'^SIND: simlocal,1,1,0' in at('AT^SIND="simlocal",1')
    assert b'^SIND: simlocal,0,1,0' in at('AT^SIND="simlocal",0')
    assert b'OK' in at('AT^SWWAN=1,1')
    assert b'^SWWAN: 1,1,1' in at('AT^SWWAN?')
    control(11,1,5,kind=1)
    mac=bytes.fromhex('020002878702')
    arp=struct.pack('!HHBBH',1,0x0800,6,4,1)+mac+socket.inet_aton('10.0.3.15')+bytes(6)+socket.inet_aton('10.0.3.2')
    frame=b'\xff'*6+mac+b'\x08\x06'+arp
    transfer([(0,len(frame),0,frame)],6)
    reply=transfer([(1,2048,0,b'')],6)[0]
    assert reply[12:14]==b'\x08\x06' and reply[20:22]==b'\x00\x02',reply.hex()
    assert reply[28:32]==socket.inet_aton('10.0.3.2')
    gateway_mac=reply[22:28]
    # Real libslirp DHCP offer, rather than a canned modem IP address.
    bootp=bytearray(236)
    struct.pack_into('!BBBBIHH',bootp,0,1,1,6,0,0x12345678,0,0x8000)
    bootp[28:34]=mac
    discover=bootp+bytes.fromhex('638253633501013703010306ff')
    send_frame(udp_frame(mac,b'\xff'*6,'0.0.0.0','255.255.255.255',68,67,discover))
    offer=recv_frame()
    assert offer[12:14]==b'\x08\x00',offer.hex()
    iplen=(offer[14]&15)*4
    payload=offer[14+iplen+8:]
    assert payload[4:8]==bytes.fromhex('12345678')
    assert payload[16:20]==socket.inet_aton('10.0.3.15'),payload.hex()
    # Full-size and exact USB packet boundaries must preserve Ethernet frames.
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as echo:
        echo.bind(('127.0.0.1',0));echo.settimeout(3)
        for length in (470,982,1400):
            payload=bytes(i%251 for i in range(length))
            send_frame(udp_frame(mac,gateway_mac,'10.0.3.15','10.0.3.2',12345,
                                 echo.getsockname()[1],payload))
            data,peer=echo.recvfrom(2048)
            assert data==payload
            echo.sendto(data,peer)
            response=recv_frame()
            assert response[42:]==payload,(len(response),length)
    r.monitor('set_link',dict(name='modem',up=False))
    assert b'^SWWAN: 1,0,1' in at('AT^SWWAN?')
    assert b'ERROR' in at('AT^SWWAN=1,1')
    r.monitor('set_link',dict(name='modem',up=True))
    assert b'OK' in at('AT^SWWAN=1,1')
    assert b'OK' in at('AT+CFUN=0')
    assert b'^SWWAN: 1,0,1' in at('AT^SWWAN?')
    print('PASS:',args.mode,'Cinterion USB, SIM/registration, strict AT parsing, PDP, NAT ARP/DHCP/UDP, USB packet boundaries, link/radio teardown')
finally:
    r.close()
    temporary.cleanup()

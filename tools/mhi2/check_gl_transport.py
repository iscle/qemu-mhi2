#!/usr/bin/env python3
"""Check bridge bulk RX, acknowledgement bounds, and cross-chunk ordering."""
import os
import select
import tempfile
import subprocess
import sys
from pathlib import Path

media=Path(sys.argv[1]).resolve()
qemu=Path(__file__).resolve().parents[2]/'build/qemu-system-arm'
scratch=tempfile.TemporaryDirectory(prefix="mhi2-gltest-")
pipe_path=Path(scratch.name)/'bridge'
os.mkfifo(str(pipe_path)+'.in');os.mkfifo(str(pipe_path)+'.out')
host=os.open(str(pipe_path)+'.in',os.O_RDWR|os.O_NONBLOCK)
tx=os.open(str(pipe_path)+'.out',os.O_RDWR|os.O_NONBLOCK)
p=subprocess.Popen([str(qemu),'-M','mhi2-harman,iram='+str(media/'iram.bin'),
    '-bios',str(media/'nor.bin'),'-display','none','-serial','null','-monitor','none',
    '-S','-qtest','stdio','-chardev',f'pipe,id=glbridge,path={pipe_path}'],
    stdin=subprocess.PIPE,stdout=subprocess.PIPE,
    stderr=open("/tmp/mhi2-gl-transport-check.log","w"),bufsize=0,env=dict(os.environ,TMPDIR=str(media/'tmp')))
pending=bytearray()
def q(cmd):
    p.stdin.write((cmd+'\n').encode())
    while b'\n' not in pending:
        assert select.select([p.stdout],[],[],5)[0],cmd
        data=os.read(p.stdout.fileno(),4096);assert data;pending.extend(data)
    line,_,rest=pending.partition(b'\n');pending[:]=rest
    assert line.startswith(b'OK'),line
    return line.split()[1:] if len(line)>2 else []
def read(addr):return int(q(f'readl {addr:#x}')[0],16)
def write(addr,value):q(f'writel {addr:#x} {value:#x}')
base=0x5f000000
try:
    assert read(base)==0x474c4252
    payload=bytes(range(256))*32
    assert os.write(host,payload)==len(payload)
    got=bytearray()
    for _ in range(10000):
        available=read(base+8)
        if not available:continue
        write(base+20,4097)
        assert read(base+8)>=available,'Oversized acknowledgement consumed bytes'
        chunk=min(available,508)
        for off in range(0,chunk,4):got+=read(base+0x2000+off).to_bytes(4,'little')
        write(base+20,chunk)
        if len(got)==len(payload):break
    assert bytes(got)==payload,(len(got),len(payload))
    assert read(base+8)==0
    assert read(base+0x2000)==0,'Read beyond available RX data'
    assert read(base+16)==0 and read(base+16)==1
    write(base+16,0);assert read(base+16)==0
    bulk_size=read(base+24)
    assert bulk_size==1024*1024
    bulk=0x5e000000
    q(f'write {bulk:#x} {len(payload)} 0x{payload.hex()}')
    write(base+28,bulk_size+1)
    assert not select.select([tx],[],[],0)[0], 'Oversized bulk TX accepted'
    write(base+28,len(payload))
    sent=bytearray()
    while len(sent)<len(payload):
        assert select.select([tx],[],[],5)[0]
        sent.extend(os.read(tx,len(payload)-len(sent)))
    assert bytes(sent)==payload
    os.write(host,payload)
    for _ in range(10000):
        if read(base+32)==len(payload):break
    assert read(base+32)==len(payload)
    write(base+36,len(payload)+1)
    assert read(base+32)==len(payload)
    result=q(f'read {bulk+bulk_size:#x} {len(payload)}')[0]
    assert bytes.fromhex(result.decode().removeprefix('0x'))==payload
    write(base+36,len(payload))
    assert read(base+32)==0
    print('PASS: bulk graphics RX ordering, bounded acknowledgements, empty reads and lock')
finally:
    os.close(host);os.close(tx);p.terminate()
    try:p.wait(timeout=5)
    except subprocess.TimeoutExpired:p.kill();p.wait()
    scratch.cleanup()

from pathlib import Path
import struct,zlib
root=Path('/home/iscle/Downloads/mhi2-analysis');raw=bytearray((root/'extracted/MMX2/mifs-stage1/70/default/mifs-stage1.img').read_bytes());p=bytearray(zlib.decompress(raw[0x800:]));base=8+struct.unpack_from('<I',p,8+32)[0];assert p[base:base+7]==b'imagefs';pos=base+struct.unpack_from('<I',p,base+16)[0]
script = None
while True:
 n=struct.unpack_from('<H',p,pos)[0];assert n
 mode=struct.unpack_from('<I',p,pos+8)[0]
 if mode&0xf000==0x8000:
  name=bytes(p[pos+32:pos+n]).split(b'\0')[0]
  if name==b'usr/sbin/startup.sh':
   off,length=struct.unpack_from('<II',p,pos+24); original=bytes(p[base+off:base+off+length]); lines=[line.strip() for line in original.splitlines() if line.strip() and not line.strip().startswith(b'#') and line.strip()!=b'tinit &']; script=b'#!/bin/ksh\n(\n'+b'\n'.join(lines)+b'\n)&\necho MHI2_DIAGNOSTIC_SHELL\nexec ksh -i\n'; assert len(script)<length;p[base+off:base+off+length]=script+b' '*(length-len(script)-1)+b'\n';break
 pos+=n
packed=zlib.compress(p,9);h=raw[:0x800];h[:8]=b'ANDROID!';struct.pack_into('<I',h,8,len(packed));struct.pack_into('<I',h,0x264,zlib.crc32(packed));struct.pack_into('<I',h,0x7fc,zlib.crc32(h[:0x7fc]));nor=bytearray((root/'qemu/k3342-70/nor.bin').read_bytes());assert len(h)+len(packed)<0x300000;nor[0x760000:0xa60000]=b'\xff'*0x300000;nor[0x760000:0x760000+len(h)+len(packed)]=h+packed;(root/'qemu/k3342-debug-nor.bin').write_bytes(nor);print(len(packed))

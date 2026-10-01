#!/usr/bin/env python3
"""Extract the LZOZ main IFS and regular files for inspection."""
import argparse,ctypes,ctypes.util,json,struct
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('image',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args()
d=a.image.read_bytes();assert d[:4]==b'LZOZ';block=struct.unpack_from('<I',d,4)[0]
lib=ctypes.CDLL(ctypes.util.find_library('lzo2'));buf=ctypes.create_string_buffer(block)
off=0x800;chunks=[]
for pos in range(8,0x800,4):
 size=struct.unpack_from('<I',d,pos)[0]
 if size==0:break
 assert off+size<=len(d)
 n=ctypes.c_size_t(block);r=lib.lzo1z_decompress_safe(d[off:off+size],ctypes.c_size_t(size),buf,ctypes.byref(n),None)
 assert r==0,(pos,off,size,r)
 chunks.append(buf.raw[:n.value]);off=(off+size+511) & ~511
raw=b''.join(chunks);assert raw[:7]==b'imagefs'
a.output.mkdir(parents=True,exist_ok=True);(a.output/'imagefs.bin').write_bytes(raw)
off=struct.unpack_from('<I',raw,16)[0];rows=[]
while True:
 size=struct.unpack_from('<H',raw,off)[0]
 if size==0:break
 assert size>=24
 inode,mode=struct.unpack_from('<II',raw,off+4)
 if mode&0xf000==0x8000:
  start,length=struct.unpack_from('<II',raw,off+24);name=raw[off+32:off+size].split(b'\0')[0].decode()
  assert not name.startswith('/') and '..' not in Path(name).parts
  dest=a.output/'files'/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(raw[start:start+length]);rows.append(dict(name=name,offset=start,size=length,inode=inode))
 off+=size
(a.output/'files.json').write_text(json.dumps(rows,indent=2)+'\n');print(len(raw),'bytes;',len(rows),'files')

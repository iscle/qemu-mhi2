#!/usr/bin/env python3
"""Check translated native fog and road shaders against decoded ALU arithmetic.

Requires captured K3342 binaries; override MHI2_SHADER_CAPTURE and MHI2_GLHOST.
"""
import os,struct,subprocess
from pathlib import Path
ROOT=Path(os.environ.get('MHI2_SHADER_CAPTURE','/home/iscle/Downloads/mhi2-analysis/qemu/navigation-capture'))
HOST=os.environ.get('MHI2_GLHOST','/tmp/mhi2-glhost')
def u(*v):return struct.pack('<'+'I'*len(v),*v)
def f(*v):return struct.pack('<'+'f'*len(v),*v)
for pair,expected in [((14,15),(128,77,89,255)),((58,59),(51,38,41,255))]:
 p=subprocess.Popen([HOST],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=open('/tmp/mhi2-shader-check.log','wb'),env=dict(os.environ,MHI2_GL_CAPTURE='/tmp/mhi2-shader-check.bin'))
 def send(op,data=b''):
  p.stdin.write(u(op,len(data))+data);p.stdin.flush()
 def query(op,data):send(op,data);return struct.unpack('<i',p.stdout.read(4))[0]
 def loc(name):return query(101,u(3,len(name))+name.encode())
 send(1,u(32,32))
 for shader,index,kind in [(1,pair[0],0x8b31),(2,pair[1],0x8b30)]:
  binary=(ROOT/f'mhi2-shader-{index}-890b.bin').read_bytes()
  send(7,u(shader,kind));send(110,u(1,0x890b,len(binary),shader)+binary)
  assert query(102,u(shader,0x8b81))==1
 send(10,u(3));send(11,u(3,1));send(11,u(3,2))
 send(14,u(3,0,len('a_position'))+b'a_position');send(12,u(3));assert query(103,u(3,0x8b82))==1
 reflected={}
 for index in range(query(103,u(3,0x8b89))):
  send(123,u(3,index,128));length,size,kind=struct.unpack('<3I',p.stdout.read(12));name=p.stdout.read(128)[:length].decode();reflected[name]=(size,kind)
 # The original binary declares vec4 even when instructions use only xyz.
 assert reflected['a_position']==(1,0x8b52),reflected
 if pair[0]==58:assert reflected['a_custom']==(1,0x8b52),reflected
 send(13,u(3))
 def uniform(name,op,values):
  at=loc(name)
  if at>=0:send(op,u(at,1)+f(*values))
 send(28,u(loc('u_mvpMatrix'),1,0)+f(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1))
 uniform('u_color',25,(.8,.2,.1,.25));uniform('u_fogColor',24,(.2,.4,.6))
 uniform('u_fogStart',22,(0,));uniform('u_fogEnd',22,(.5,))
 uniform('u_scaleXYZ',22,(1,));uniform('u_scaleH',23,(0,0));uniform('u_origin',24,(0,0,0))
 uniform('u_widthScale',22,(1,));uniform('u_roadGeometryWidthHalf',22,(1,))
 send(15,u(1,4));send(16,u(0x8892,4))
 vertices=f(-1,-1,0,1,-1,0,1,1,0,-1,-1,0,1,1,0,-1,1,0)
 send(17,u(0x8892,len(vertices),0x88e4)+vertices);send(19,u(0,3,0x1406,0,12,0));send(20,u(0))
 send(2,u(0,0,32,32));send(3,f(.1,.1,.1,1));send(4,u(0x4000));send(31,u(4,0,6))
 send(120,u(16,16,1,1,0x1908,0x1401));pixel=tuple(p.stdout.read(4));assert all(abs(a-b)<=2 for a,b in zip(pixel,expected)),(pair,pixel,expected)
 assert query(105,b'')==0
 p.stdin.close();assert p.wait(timeout=10)==0
 print(pair,'rendered',pixel,'expected',expected)

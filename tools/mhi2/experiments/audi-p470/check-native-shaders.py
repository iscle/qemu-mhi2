#!/usr/bin/env python3
"""Compile recovered programs through the actual graphics RPC bridge."""
import os,struct,subprocess,json
from pathlib import Path
R=Path(__file__).resolve().parent
import argparse,tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--host', required=True, type=Path)
parser.add_argument('--captures', required=True, type=Path, help='Directory containing captured shader .bin files')
parser.add_argument('--cache', type=Path, default=R/'shader-cache')
args=parser.parse_args()
cache=args.cache.resolve();host=args.host.resolve()
output=tempfile.TemporaryDirectory(prefix='mhi2-native-shaders-');R=Path(output.name)
u=lambda *v:struct.pack('<'+'I'*len(v),*v)
p=subprocess.Popen([str(host)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=open(R/'shader-compile.log','wb'),env=dict(os.environ,MHI2_SHADER_CACHE=str(cache),MHI2_GL_FRAME=str(R/"shader-test-frame.ppm")))
def send(op,data=b''):
 p.stdin.write(u(op,len(data))+data);p.stdin.flush()
def query(op,data):
 send(op,data);return struct.unpack('<i',p.stdout.read(4))[0]
send(1,u(32,32));results=[]; ids={}
for i,src in enumerate(sorted(cache.glob('*.glsl')),1):
 h=src.stem.split('-')[0];ids[h]=i;binary=next(args.captures.glob(h+'-*.bin')).read_bytes()
 kind=0x8b31 if 'attribute ' in src.read_text() else 0x8b30
 send(7,u(i,kind));send(110,u(1,0x890b,len(binary),i)+binary)
 passed=query(102,u(i,0x8b81))==1;results.append({'hash':h,'compiled':passed})
pairs=[('6078d2a8d80585f7','1961f4a7ac29ba22'),('7b2b3210936ed353','ce6cef4f81e6bd86'),('7a4038b70dca7809','36d5b5d1755989e9'),('60cf06553a86e73e','730a52061eb97de5'),('15ad27bf8d42886a','fd12b5eaefc1ced2'),('7cb772d0a33c2d01','1741c600471d8e8f'),('ae76d915ea796648','8e737d5293e50f53'),('ed1927b6db5523d8','352ad410c772a062'),('51ed86f9a97c8df4','04fb3f2c9d9b8aaf'),('76fcea54bb7b64f9','08e42ce15ee05876'),('28b3e673ff20c22a','f1f72e0c4d5c49fb'),('97da0a761acf6ba1','d5e8bb34f94b434c'),('8fa81840936d9d7b','bcac1d79ebe5e35b')]
pairs += [('02a991bfc74d6f29',h) for h in ('c05313b981de9f9e','14e87a9476a80d3c','ed3a1d9bc7560c05','c9329494e13778d4')]
for program,(vs,fs) in enumerate(pairs,100):
 send(10,u(program));send(11,u(program,ids[vs]));send(11,u(program,ids[fs]));send(14,u(program,0,len('a_position'))+b'a_position');send(12,u(program))
 assert query(103,u(program,0x8b82))==1,(vs,fs)
print(f'{len(pairs)} native pairs linked')
# Render the road extrusion program using identity transforms and compare the
# premultiplied source-over result against the decoded destination-read ALU.
f=lambda *v:struct.pack('<'+'f'*len(v),*v)
program=102
send(13,u(program))
def loc(name):return query(101,u(program,len(name))+name.encode())
def uniform(name,op,values):
 location=loc(name)
 if location>=0:send(op,u(location,1)+f(*values))
send(28,u(loc('u_mvpMatrix'),1,0)+f(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1))
uniform('u_scaleXYZ',22,(1,));uniform('u_scaleH',23,(0,0));uniform('u_origin',24,(0,0,0))
uniform('u_roadGeometryWidthHalf',22,(0,));uniform('u_widthScale',22,(0,))
uniform('u_color',25,(.2,.1,.05,.25))
send(15,u(1,4));send(16,u(0x8892,4))
vertices=f(-1,-1,0,1,-1,0,1,1,0,-1,-1,0,1,1,0,-1,1,0)
send(17,u(0x8892,len(vertices),0x88e4)+vertices);send(19,u(0,3,0x1406,0,12,0));send(20,u(0))
send(2,u(0,0,32,32));send(3,f(.1,.1,.1,1));send(4,u(0x4000));send(31,u(4,0,6))
send(120,u(16,16,1,1,0x1908,0x1401));pixel=tuple(p.stdout.read(4));expected=(70,45,32,255)
assert all(abs(a-b)<=2 for a,b in zip(pixel,expected)),(pixel,expected)
assert query(105,b'')==0
print('Road geometry and source-over pixel:',pixel)

# Validate the textured billboard path, including its FX10 color lanes and
# premultiplied blend. No map content or style constants are substituted.
program=113
send(13,u(program))
send(28,u(loc('u_mvpMatrix'),1,0)+f(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1))
uniform('u_texCoordScaleXY',23,(1,1))
uniform('u_colorScale',25,(.5,.1,.8,.6))
send(50,u(1,5));send(51,u(0x0de1,5))
send(52,u(0x0de1,0,0x1908,1,1,0,0x1908,0x1401,4)+bytes((51,77,102,128)))
send(53,u(0x0de1,0x2801,0x2600));send(53,u(0x0de1,0x2800,0x2600))
send(3,f(.1,.1,.1,1));send(4,u(0x4000));send(31,u(4,0,6))
send(120,u(16,16,1,1,0x1908,0x1401));pixel=tuple(p.stdout.read(4))
expected=(43,26,99,255)
assert all(abs(a-b)<=2 for a,b in zip(pixel,expected)),(pixel,expected)
print('Billboard color and alpha pixel:',pixel)
send(52,u(0x0de1,0,0x1908,1,1,0,0x1908,0x1401,4)+bytes((255,255,255,0)))
send(3,f(.1,.1,.1,1));send(4,u(0x4000));send(31,u(4,0,6))
send(120,u(16,16,1,1,0x1908,0x1401));pixel=tuple(p.stdout.read(4))
assert pixel==(26,26,26,255),pixel
print('Transparent billboard preserves map pixel:',pixel)

p.stdin.close();p.wait(timeout=10)
(R/'shader-compile.json').write_text(json.dumps(results,indent=2)+'\n')
print(f'{sum(x["compiled"] for x in results)}/{len(results)} compiled')
assert all(x['compiled'] for x in results),[x for x in results if not x['compiled']]

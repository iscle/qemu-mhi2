#!/usr/bin/env python3
"""Translate decoded Tegra vertex arithmetic, preserving reflected uniforms."""
import json,struct,re
from pathlib import Path
ROOT=Path(__file__).resolve().parent

TYPES={0x1406:('float',1),0x8b50:('vec2',2),0x8b51:('vec3',3),0x8b52:('vec4',4),0x8b5c:('mat4',16),0x8b5e:('sampler2D',0)}

def metadata(b):
 w=struct.unpack('<'+'I'*(len(b)//4),b)
 ss={t:(o*4,n,s) for t,o,n,s in [w[5+i*4:9+i*4] for i in range(w[4])]}
 so=ss[1][0]
 def name(off):
  off+=so;return b[off:b.index(b'\0',off)].decode()
 attrs=[];vary=[];uniforms=[]
 o,n,size=ss.get(2,(0,0,0))
 for i in range(n):
  r=struct.unpack_from('<4I',b,o+16*i);attrs.append((name(r[0]),TYPES[r[1]],r[2]))
 o,n,size=ss.get(3,(0,0,0))
 for i in range(n):
  r=struct.unpack_from('<6I',b,o+24*i);vary.append((name(r[0]),TYPES[r[3]],r[5]))
 o,n,size=ss[8];bo=ss[13][0]
 for i in range(n):
  r=struct.unpack_from('<8I',b,o+32*i);packed=struct.unpack_from('<I',b,bo+8*i)[0]
  uniforms.append((name(r[0]),TYPES[r[1]],(packed>>16)//4,packed))
 return ss,attrs,vary,uniforms

def operand(text):
 text=re.sub(r'([rcvo])(\d+)',lambda m: {'r':'r','c':'c','v':'v','o':'o'}[m[1]]+'['+m[2]+']',text)
 return text

def vertex(b,asm):
 ss,attrs,vary,uniforms=metadata(b)
 lines=['precision highp float;']
 for name,(kind,count),reg in attrs:lines.append(f'attribute {kind} {name};')
 for name,(kind,count),reg in vary:lines.append(f'varying mediump {kind} {name};')
 for name,(kind,count),reg,packed in uniforms:lines.append(f'uniform {kind} {name};')
 lines+=['void main(){vec4 r[64];vec4 c[64];vec4 v[16];vec4 o[16];vec4 t;','for(int i=0;i<64;i++){r[i]=vec4(0.0);c[i]=vec4(0.0);}for(int i=0;i<16;i++){v[i]=vec4(0.0);o[i]=vec4(0.0);}']
 if 18 in ss:
  off,n,size=ss[18];floats=struct.unpack_from('<'+'f'*(size//4),b,off)
  for i in range(0,len(floats),4):
   group=floats[i:i+4]
   if any(group):lines.append(f'c[{i//4}]=vec4('+','.join(f'{x:.9g}' if '.' in f'{x:.9g}' or 'e' in f'{x:.9g}' else f'{x:.9g}.0' for x in group)+');')
 for name,(kind,count),reg,packed in uniforms:
  if kind=='sampler2D':continue
  if kind=='mat4':
   for col in range(4):lines.append(f'c[{reg+col}]={name}[{col}];')
  else:
   args=[name]+['0.0']*(4-count);lines.append(f'c[{reg}]=vec4('+','.join(args)+');')
 for name,(kind,count),reg in attrs:
  lines.append(f'v[{reg}]=vec4('+','.join([name]+['0.0']*(3-count)+(['1.0'] if count<4 else []))+');')
 ops=[]
 for line in asm.splitlines():
  m=re.match(r'\s+(mov|mul|add|mad|dp3|dp4|min|max|frc|flr|rcp|rsq|lg2|ex2|sin|cos) ([ro]\d+)(_sat)?\.([xyzw]+), (.*)',line)
  if not m:continue
  op,dest,sat,mask,args=m.groups();a=[operand(x) for x in args.split(', ')]
  expressions={'mov':lambda:a[0],'mul':lambda:f'({a[0]})*({a[1]})','add':lambda:f'({a[0]})+({a[1]})','mad':lambda:f'({a[0]})*({a[1]})+({a[2]})','dp3':lambda:f'vec4(dot(({a[0]}).xyz,({a[1]}).xyz))','dp4':lambda:f'vec4(dot({a[0]},{a[1]}))','min':lambda:f'min({a[0]},{a[1]})','max':lambda:f'max({a[0]},{a[1]})','frc':lambda:f'fract({a[0]})','flr':lambda:f'floor({a[0]})','rcp':lambda:f'vec4(1.0/({a[0]}))','rsq':lambda:f'vec4(inversesqrt({a[0]}))','lg2':lambda:f'vec4(log2({a[0]}))','ex2':lambda:f'vec4(exp2({a[0]}))','sin':lambda:f'vec4(sin({a[0]}))','cos':lambda:f'vec4(cos({a[0]}))'}
  expr=expressions[op]()
  if sat:expr=f'clamp({expr},0.0,1.0)'
  lines.append(f't=vec4({expr});{operand(dest)}.{mask}=t.{mask};');ops.append(op)
 # Every emitted assembly instruction must have been translated; reject unsupported opcodes.
 expected=sum('        ' in l and re.search(r' [ro]\d+(?:_sat)?\.',l)!=None for l in asm.splitlines())
 if len(ops)!=expected:raise ValueError(f'unsupported vertex instructions {len(ops)}/{expected}')
 lines.append('gl_Position=o[0];')
 for name,(kind,count),reg in vary:lines.append(f'{name}=o[{reg}].'+ 'xyzw'[:count]+';')
 if 'u_pointSize' in [x[0] for x in uniforms]:lines.append('gl_PointSize=u_pointSize;')
 lines.append('}')
 return '\n'.join(lines)+'\n'

if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('captures', type=Path, help='Directory containing unsupported-shaders.json and decoded .asm files')
 parser.add_argument('--output', type=Path, default=ROOT/'shader-cache')
 args=parser.parse_args(); ROOT=args.captures
 DATA=json.loads((ROOT/'unsupported-shaders.json').read_text())
 out=args.output;out.mkdir(exist_ok=True, parents=True)
 for h,(path,size) in DATA.items():
  asm=(ROOT/f'{h}.asm').read_text()
  if not asm.startswith('  instructions:'):continue
  try:
   src=vertex(Path(path).read_bytes(),asm)
   (out/f'{h}-{size}.glsl').write_text(src);(out/f'{h}-{size}.blend').write_text('0\n');print(h,'translated')
  except Exception as exc:print(h,exc)

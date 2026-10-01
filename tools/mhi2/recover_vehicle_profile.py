from pathlib import Path
import re,json
import argparse
parser=argparse.ArgumentParser(description='Recover K3342 diagnostic defaults; select simulated vehicle equipment')
parser.add_argument('decompiled',type=Path)
root=parser.parse_args().decompiled/'de/vw/mib/asl/framework/internal/diagnosis/variant/high2'
result={}
for p in root.glob('*Impl.java'):
 s=p.read_text();m=re.search(r'super\(byArray, new int\[\]\{([^}]+)\}, (\d+), (\d+)L\)',s)
 if not m:continue
 size=int(m[1].split(',')[-1]);data=bytearray(size)
 raw=re.search(r'this.mConfiguration = new int\[\]\{([^}]+)\}',s)
 if not raw: continue
 a=[int(x.strip().replace('Integer.MIN_VALUE','-2147483648').replace('Short.MAX_VALUE','32767').replace('Integer.MAX_VALUE','2147483647'),0) for x in raw[1].split(',')]
 start=a[-1]; fields={a[i]:a[i+1] for i in range(start+1,start+1+a[start],2) if a[i]>=0}
 def put(fid,value):
  pos=fields[fid]; typ,bits,off,bit,default=a[pos:pos+5]
  if typ in (10037,20051):
   if bits<=8:
    mask=((1<<bits)-1)<<bit;data[off]=(data[off]&~mask)|((int(value)<<bit)&mask)
   elif bits%8==0 and bit==0:data[off:off+bits//8]=int(value).to_bytes(bits//8,'big')
  elif typ==30029 and isinstance(value,bytes):data[off:off+bits//8]=value[:bits//8].ljust(bits//8,b' ')
 for fid,pos in fields.items():
  if a[pos] in (10037,20051):
   try:put(fid,a[pos+4])
   except (ValueError,OverflowError,IndexError):pass
 if p.stem=='CarFuncAdapImpl':
  for fid in (240,255,385):put(fid,1)
  for fid in (242,257,387):put(fid,1)
 if p.stem=='CodingImpl':
  for fid,value in {5:1,97:0,83:1,105:1}.items():put(fid,value)
 if p.stem=='AdaptationImpl':
  # Native SmartphoneIntegration ASLEventHandler reads these equipment flags:
  # Google Automotive Link, Apple CarPlay, MirrorLink.
  for fid in (1142,1143,1169):put(fid,1)
 if p.stem=='DashboardDisplayConfigImpl':
  # H.264 streaming to an emulated MOST cluster (K3342 field definitions).
  for fid,value in {1172:2,171:1,1176:1}.items():put(fid,value)
 if p.stem=='IdentificationImpl':
  for fid,value in {839:b'SIM-QEMU',840:b'MHI2 K3342',841:b'1427',842:b'SIM'}.items():put(fid,value)
 result[f'{m[2]}:{m[3]}']={'type':'blob','value':data.hex(),'source':p.stem+' defaults; simulated unit'}
# Information is an integer persistence record, despite the diagnosis byte view.
# Native media extracts coding state from bits 8..15 and profile from bits 0..7.
result['678364556:21']={'type':'int','value':0x101,'source':'Native media CLastModeManagerJobStart::readProfile; simulated coded profile 1'}
for k,x in {'30:1966083':'1427','30:1966084':'MHI2_ER_VWG11_K3342'}.items():
 result[k]={'type':'string','value':x,'source':'firmware metainfo2.txt'}
result['46924065:401']={'type':'blob','value':b'MHI2_ER_VWG11_K3342'.hex(),'source':'firmware metainfo2.txt'}
Path(__file__).with_name('vehicle_profile.json').write_text(json.dumps(result,indent=2)+'\n')
print('Recovered',len(result),'attributes')

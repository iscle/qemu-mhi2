from pathlib import Path
import re,json
import argparse
parser=argparse.ArgumentParser(description='Recover firmware wire metadata from CFR output')
parser.add_argument('decompiled',type=Path)
parser.add_argument('--output',type=Path,default=Path(__file__).with_name('dsi_schema.json'))
args=parser.parse_args()
root=args.decompiled
out={'services':{},'structs':{}}
for p in root.rglob('*Serializer.java'):
 s=p.read_text(); name=p.stem[:-10]
 # Field assignments from the deserializer define the exact wire order.
 fields=[]
 for line in s.splitlines():
  m=re.search(r'\w+\.(\w+) = .*?(?:iDeserializer|\w+Serializer)\.get(\w+)\((?:iDeserializer)?\);',line)
  if m: fields.append(list(m.groups()))
 if fields: out['structs'][name]=fields
for p in root.rglob('*Proxy.java'):
 if '$' in p.name: continue
 s=p.read_text(); m=re.search(r'new ServiceInstanceID\("([^"]+)", n, "([^"]+)", "([^"]+)"',s)
 if not m: continue
 name=p.stem[:-5]; d={'uuid':m[1],'key':m[2],'calls':{},'replies':{},'attrs':{}}
 for b in re.split(r'\n    public void ',s)[1:]:
  header=b.split('{',1)[0]; call=re.search(r'remoteCallMethod\(\(short\)(\d+)',b)
  if call: d['calls'][call[1]]=header.strip().split(' throws')[0]
 rp=p.with_name(name+'ReplyService.java')
 if not rp.exists(): continue
 for mid,body in re.findall(r'case (\d+): \{(.*?)\n\s+break;',rp.read_text(),re.S):
  mm=re.search(r'this\.p_\w+\.(\w+)\(',body)
  if not mm: continue
  types=re.findall(r'(?:iDeserializer|\w+Serializer)\.get(\w+)\((?:iDeserializer)?\)',body)
  d['replies'][mid]={'name':mm[1],'types':types}
 ip=root/'org/dsi/ifc'/p.parent.parent.name/(name+'.java')
 if ip.exists():
  d['attrs']={num:attr for attr,num in re.findall(r'int ATTR_(\w+) = (\d+)',ip.read_text())}
  requests=dict(re.findall(r'int RT_(\w+) = (\d+)',ip.read_text()))
  d['request_ids']={mid:int(requests[signature.split('(')[0].upper()])
                    for mid,signature in d['calls'].items()
                    if signature.split('(')[0].upper() in requests}
 out['services'][name]=d
args.output.write_text(json.dumps(out,indent=2)+'\n')
print({k:(len(v['calls']),len(v['replies'])) for k,v in out['services'].items()},len(out['structs']))

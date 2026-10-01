"""RCC ASI Attributes persistence backend, ESO serializer 1.

The initial data is the explicit simulated vehicle profile. Writes survive for
this emulator session, matching the VM's default snapshot mode.
"""
import json
import struct
from pathlib import Path


def array(values, fmt='I'):
    return b'\0'+struct.pack('!I',len(values))+b''.join(struct.pack('!'+fmt,v) for v in values)


class Reader:
    def __init__(self,data): self.data=data; self.pos=0
    def take(self,n):
        if n<0 or self.pos+n>len(self.data): raise ValueError('Truncated persistence request')
        value=self.data[self.pos:self.pos+n];self.pos+=n;return value
    def array(self,fmt='I'):
        if self.take(1)!=b'\0':return []
        n=struct.unpack('!I',self.take(4))[0]
        if n>131071: raise ValueError('Oversized persistence array')
        return [struct.unpack('!'+fmt,self.take(struct.calcsize(fmt)))[0] for _ in range(n)]


class Persistence:
    def __init__(self):
        self.values=json.loads(Path(__file__).with_name('vehicle_profile.json').read_text())

    def handle(self,mid,data):
        if mid==9:return []
        r=Reader(data);namespaces=r.array();keys=r.array()
        if len(namespaces)!=len(keys):raise ValueError('Mismatched persistence keys')
        ids=[f'{n}:{k}' for n,k in zip(namespaces,keys)]
        prefix=array(namespaces)+array(keys)
        print('RCC PERSIST',mid,ids,flush=True)
        if mid==8:return [(10,prefix+array([0]*len(ids),'i'))]
        if mid==7:
            replies=[]
            for kind,reply in [('blob',0),('int',1),('string',6)]:
                entries=[(n,k,self.values.get(i)) for n,k,i in zip(namespaces,keys,ids) if self.values.get(i,{}).get('type','blob')==kind]
                if not entries:continue
                header=array([n for n,k,v in entries])+array([k for n,k,v in entries])
                values=[v['value'] if v else '' for n,k,v in entries]
                if kind=='blob':payload=b'\0'+struct.pack('!I',len(values))+b''.join(array(bytes.fromhex(v),'B') for v in values)
                elif kind=='int':payload=array(values,'i')
                else:
                    payload=b'\0'+struct.pack('!I',len(values))+b''.join(b'\0\0'+struct.pack('!H',len(v.encode()))+v.encode() for v in values)
                replies.append((reply,header+payload+array([0 if v else 1 for n,k,v in entries],'i')))
            return replies
        if mid==2:
            if r.take(1)!=b'\0':raise ValueError('Null blob values')
            count=struct.unpack('!I',r.take(4))[0]
            if count!=len(ids):raise ValueError('Mismatched blob values')
            values=[bytes(r.array('B')).hex() for _ in ids];kind='blob'
        elif mid==3:values=r.array('i');kind='int'
        elif mid==5:
            if r.take(1)!=b'\0':raise ValueError('Null string values')
            count=struct.unpack('!I',r.take(4))[0]
            if count!=len(ids):raise ValueError('Mismatched string values')
            values=[];kind='string'
            for _ in ids:
                if r.take(1)!=b'\0':values.append('');continue
                encoding=r.take(1)[0];length=struct.unpack('!H',r.take(2))[0]
                if encoding!=0:raise ValueError('Unsupported string encoding')
                values.append(r.take(length).decode('utf-8'))
        else:raise ValueError('Unknown persistence method')
        if len(values)!=len(ids) or r.pos!=len(data):raise ValueError('Invalid persistence values')
        for key,value in zip(ids,values):self.values[key]={'type':kind,'value':value}
        return [(4,prefix+array([0]*len(ids),'i'))]+self.handle(7,prefix)

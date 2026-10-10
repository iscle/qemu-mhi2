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
    def __init__(self, config=None, metadata=None):
        from firmware_profile import common_metadata
        metadata = common_metadata() if metadata is None else metadata
        from skoda_profile import RELEASE as SKODA_RELEASE, validate_metadata, firmware_identity
        self.read_only = metadata.get('release') == SKODA_RELEASE
        if self.read_only:
            validate_metadata(metadata)
            self.values = firmware_identity()
            self.identity = None
            config = None
        else:
            self.values=json.loads(Path(__file__).with_name('vehicle_profile.json').read_text())
            from emulator_config import load, validate
            config = load() if config is None else validate(config)
            self.identity = config['identity'].copy()
            self.values['0:3221291024'] = dict(
                type='blob', value=self.identity['fazit_id'].encode('utf-8').hex())
        if metadata.get('release') == 'MHI2_ER_POG11_K5126':
            self.configure_porsche(config)
        for key, field in [('30:1966084', 'release'), ('30:1966083', 'MUVersion')]:
            if metadata.get(field):
                self.values[key] = dict(type='string', value=metadata[field])
        if metadata.get('release'):
            # Native GAL ignores train blobs shorter than 21 bytes, then
            # trims whitespace (onIdentificationMuTrainChanged, 0x18d81c).
            self.values['46924065:401'] = dict(
                type='blob', value=metadata['release'].encode().ljust(32, b' ').hex())

    def configure_porsche(self, config):
        # TelephoneDiagnosis::diagCBDiagnosisPModeValues reads byte 1 bit 3
        # after validating a minimum three-byte diagnostic value. An absent
        # value leaves NAD/SIM handling in factory production mode.
        self.values['0:3221356586'] = dict(type='blob', value='000000')
        # ConnectionManager::activateDataService requires exactly one byte.
        self.values['0:3221356610'] = dict(
            type='blob', value='01' if config['features']['00060700'] else '00')
        # K5126 smartphone_integrator: coding callback 0x193968 and
        # adaptation callback 0x19377c. USB mode other than 3 forcibly clears
        # the port mask (0x193448), independently of the FEC permission.
        coding = bytearray.fromhex(self.values['28180695:1']['value'])
        adaptation = bytearray.fromhex(self.values['28442848:100']['value'])
        # GAL CCodingProvider::onCodingChanged (0x18de98) reads the brand
        # from byte 0's low nibble; its enum printer (0x14e398) maps 7 to
        # PORSCHE. Do not inherit VW identity from the generic fixture.
        coding[0] = (coding[0] & 0xf0) | 7
        coding[19] = (coding[19] & 0x3f) | 0xc0
        coding[24] |= 0x08  # Soldered Marvell WLAN module is present.
        adaptation[15] = 1  # Telephone function, including the onboard NAD.
        adaptation[16] = 1  # Native ConnectionManager WLAN enable byte.
        adaptation[30] |= 1  # First modeled smartphone USB port.
        # High-platform AdaptationImplHigh: POI, portal, Earth/Street View,
        # account integration; picture destinations, dictation, RemoteHMI,
        # online metadata; online media and WLAN client. These are equipment
        # flags, not server-side service activation or account credentials.
        for index, mask in ((17, 0xbe), (18, 0x37), (43, 0x10), (51, 0x04)):
            adaptation[index] = ((adaptation[index] & ~mask) |
                                 (mask if config['features']['00060700'] else 0))
        for index, mask, feature in [(43, 0x80, '00060900'),
                                     (51, 0x01, '00060800'),
                                     (51, 0x60, '00060300'),
                                     (43, 0x40, '00060b00'),
                                     (70, 0x04, '00060b00')]:
            # Preserve unrelated vehicle adaptation bits. Select MirrorLink's
            # existing bit when enabled, or its first mode for a blank fixture.
            selected = (adaptation[index] & mask) or (mask & -mask)
            adaptation[index] = (adaptation[index] & ~mask) | (selected if config['features'][feature] else 0)
        self.values['28180695:1']['value'] = coding.hex()
        self.values['28442848:100']['value'] = adaptation.hex()

    def handle(self,mid,data):
        if mid==9:return []
        r=Reader(data);namespaces=r.array();keys=r.array()
        if len(namespaces)!=len(keys):raise ValueError('Mismatched persistence keys')
        ids=[f'{n}:{k}' for n,k in zip(namespaces,keys)]
        prefix=array(namespaces)+array(keys)
        print('RCC PERSIST',mid,ids,flush=True)
        if mid==8:return [(10,prefix+array([1 if self.read_only else 0]*len(ids),'i'))]
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
        if self.read_only:
            return [(4,prefix+array([1]*len(ids),'i'))]
        for key,value in zip(ids,values):self.values[key]={'type':kind,'value':value}
        return [(4,prefix+array([0]*len(ids),'i'))]+self.handle(7,prefix)

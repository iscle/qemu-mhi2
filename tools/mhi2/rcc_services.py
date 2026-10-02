"""MHI2 ESO service models for the local, simulated vehicle.

Wire IDs/layouts are recovered from the firmware's generated DSI serializers.
Vehicle values describe an emulated parked car, not an actual vehicle identity.
"""
import json
import hashlib
import re
import struct
import uuid
import copy
from datetime import datetime, timezone
from pathlib import Path
from rcc_persistence import Persistence, Reader
from rcc_features import verify_data_signature, FEATURES

SCHEMA = json.loads(Path(__file__).with_name('dsi_schema.json').read_text())
FORMATS = {'Int8':'b', 'UInt8':'B', 'Int16':'h', 'UInt16':'H',
           'Int32':'i', 'UInt32':'I', 'Int64':'q', 'UInt64':'Q',
           'Float':'f', 'Double':'d', 'Bool':'?'}


def encode(kind, value=None):
    if kind == 'Bool':
        # ESO DefaultSerializerBase uses 0xff for true (readers accept nonzero).
        return b'\xff' if value else b'\0'
    if kind.startswith('Optional'):
        kind = kind[8:]
        if kind.endswith('VarArray'):
            values = [] if value is None else value
            element = kind[:-8]
            if element not in FORMATS:
                element = 'Optional' + element
            return b'\0' + struct.pack('!I', len(values)) + b''.join(encode(element, v) for v in values)
        if kind == 'String':
            data = ('' if value is None else value).encode('utf-8')
            return b'\0\0' + struct.pack('!H', len(data)) + data
        fields = SCHEMA['structs'][kind]
        values = {} if value is None else value
        return b'\0' + b''.join(encode(t, values.get(n)) for n,t in fields)
    return struct.pack('!'+FORMATS[kind], 0 if value is None else value)


def decode(kind, reader):
    if kind.startswith('Optional'):
        kind=kind[8:]
        if reader.take(1)!=b'\0': return None
        if kind.endswith('VarArray'):
            n=struct.unpack('!I',reader.take(4))[0]
            if n>4096:raise ValueError('Oversized DSI array')
            element=kind[:-8]
            if element not in FORMATS:element='Optional'+element
            return [decode(element,reader) for _ in range(n)]
        if kind=='String':
            encoding=reader.take(1)[0];n=struct.unpack('!H',reader.take(2))[0]
            if encoding!=0:raise ValueError('Unsupported DSI string encoding')
            return reader.take(n).decode('utf-8')
        return {f:decode(t,reader) for f,t in SCHEMA['structs'][kind]}
    return struct.unpack('!'+FORMATS[kind],reader.take(struct.calcsize(FORMATS[kind])))[0]


def inventory():
    # Only MMX/RCC payloads in the booted firmware; optional archive components
    # (tuners, amplifiers, phones) are not asserted to be installed.
    from firmware_profile import current
    path = Path(current()['metadata'])
    devices = {}
    if not path.exists():return []
    sections={};current={}
    for line in path.read_text().splitlines():
        if line.startswith('['):
            current=sections.setdefault(line[1:line.index(']')],{})
        else:
            match=re.match(r'(\w+)\s*=\s*"(.*)"',line)
            if match:current[match[1]]=match[2]
    def resolve(section,seen):
        if section in seen:raise ValueError('Cyclic firmware metadata link')
        item=sections.get(section,{})
        if 'Link' in item:
            return {**resolve(item['Link'].strip('[]'),seen|{section}),**item}
        return item
    for section in sections:
        parts=section.split('\\')
        if len(parts)<5 or parts[0] not in ('MMX2','RCC'):continue
        if parts[0]=='MMX2' and parts[2]!='70':continue
        item=resolve(section,set())
        if not item.get('Version','').isdigit():continue
        devices.setdefault(parts[0],[]).append({
            'name':'/'.join(parts[1:]),'version':int(item['Version']),
            'hw':int(parts[2]) if parts[2].isdigit() else 0})
    return [{'name':n,'modules':v} for n,v in devices.items()]


class Services:
    def __init__(self):
        self.definitions = {k:v for k,v in SCHEMA['services'].items() if (k.startswith('DSI') and k not in ('DSIPersistence','DSIKombiPictureServer')) or k in ('Attributes', 'FecManager')}
        self.definitions['VideoConnection'] = {
            'uuid':'45926ef4-737d-44b0-aacd-b24a0671e42d',
            'key':'12eb7d83-c894-59a6-8d27-23be768a579d',
            'calls':{'9':'requestVideoConnection(int terminal, int label)',
                     '8':'releaseVideoConnection(int terminal, int label)',
                     '4':'requestFrameRate(int device)'},
            'replies':{},'attrs':{}}
        # Original libasimmxdisplayproxy.so: IPower UUID/key and method
        # serializers, plus ILvds. Display brightness uses signed steps -5..5.
        self.definitions['DisplayPower'] = {
            'uuid':'92c39bd3-ca7c-4d76-957b-ba8b83eb0d20',
            'key':'7900f601-13e0-5850-a98b-287e195bd326',
            'calls':{'0':'getBrightness(uint32 display)', '2':'getPower(uint32 display)',
                     '4':'setBrightness(uint32 display, int8 value)',
                     '6':'setPower(uint32 display, int32 value)'},
            'replies':{}, 'attrs':{}}
        self.definitions['DisplayLvds'] = {
            'uuid':'685c105d-4b0c-4420-9893-75e55da2600a',
            'key':'8873f7d0-bee9-5da5-85e2-ea93f5d28e6e',
            'calls':{'0':'setStatus(int32 status, uint32 label)'},
            'replies':{}, 'attrs':{}}
        self.persistence = Persistence()
        self.by_uuid = {uuid.UUID(v['uuid']).bytes:k for k,v in self.definitions.items() if k != 'DSIKeyPanel'}
        self.inventory = inventory()
        self.values = {}
        self.profiles = {}
        self.active_profiles = {}

    def registrations(self):
        for name, d in self.definitions.items():
            if name == 'DSIKeyPanel':
                continue  # Its input-event reply channel is owned by Peer.
            yield name, uuid.UUID(d['uuid']).bytes, uuid.UUID(d['key']).bytes, (1 if name in ('Attributes','VideoConnection','FecManager','DisplayPower','DisplayLvds') else 0)
            if name == 'DSIWavePlayer':
                yield name, uuid.UUID(d['uuid']).bytes, uuid.UUID(d['key']).bytes, 1

    def reply(self, name, method, values):
        d = self.definitions[name]
        mid, spec = next((int(i),v) for i,v in d['replies'].items() if v['name']==method)
        if len(values) != len(spec['types']):
            raise ValueError((name, method, values, spec))
        return mid, b''.join(encode(t,v) for t,v in zip(spec['types'], values))

    def initial(self, name, spec):
        method = spec['name']
        types = spec['types']
        vals = [None for t in types]
        vals[-1] = 129  # valid + subscription acknowledgement
        if method == 'updateProfileState' and name in ('DSISound', 'DSIAMFMTuner', 'DSIDABTuner'):
            return [2, self.active_profiles.get(name, 0), 129]
        if name == 'DSIDataConnection':
            if method == 'updateStateDataConnection':
                vals[0] = {'contextState': 4, 'operationMode': 2}  # Not attached, limited.
            elif method == 'updateRoamingState':
                vals[0] = 12
            elif method == 'updateConnectionStateInformation':
                vals[0] = {'connectionState': 24, 'applicationID': 0}  # Blocked, no modem.
        if name == 'DSIDataConfiguration':
            if method == 'updateActiveProfile': vals[0] = -1
            elif method == 'updateConnectionMode': vals[0] = 2  # Never: no modem transport.
            elif method == 'updateRoamingState': vals[0] = 1
        if name == 'DSIAudioManagement':
            if method == 'updateAMAvailable':
                vals[:2] = [3, 1]  # AVAILABLE, front-driver terminal
            elif method in ('updateActiveConnection', 'updateActiveEntertainmentConnection'):
                vals[:2] = [self.values.get(('audio', 1), 0), 1]
        if name == 'DSISound':
            if method == 'updateActiveAmplifierCapabilities':
                vals[0] = {'amplifier': 2, 'balance': True, 'fader': True,
                           'subwoofer': False, 'equalizer': 0}
            elif method.endswith('Range') and len(vals) == 3:
                vals[:2] = [0, 30] if method == 'updateVolumeRange' else [-9, 9]
            elif method == 'updateVolume':
                vals[:3] = [0, 1, 15]
        if name == 'DSISwdlDeviceInfo' and method == 'updateSummaryChanged':
            from firmware_profile import current
            vals[0] = current()['train']
        if name == 'DSIKOMOGfxStreamSink':
            vals[0]={'updateGfxState':1,'updateDataRate':2,'updateRequestSync':1}.get(method,0)
        if name == 'DSICarKombi':
            if method == 'updateBCViewOptions':
                enabled = {'currentConsumption1','currentRange1','totalDistance',
                           'shortTermAverageConsumption1','shortTermGeneral',
                           'longTermAverageConsumption1','longTermGeneral',
                           'cycleAverageConsumption1','cycleGeneral','tankLevel1',
                           'menue1Config','menue2Config','menue3Config',
                           'resetMenue1','resetMenue2','resetMenue3',
                           'vehicleStateList','outsideTemperature','digitalSpeed'}
                vals[0] = {f:{'state':2,'reason':0} for f,t in SCHEMA['structs']['BCViewOptions'] if f in enabled}
                vals[0]['configuration']={'primaryEngineType':1}
            elif method == 'updateSIAViewOptions':
                vals[0] = {f:{'state':2,'reason':0} for f,t in SCHEMA['structs']['SIAViewOptions'] if t=='OptionalCarViewOption'}
        elif name == 'DSICarVehicleStates' and method == 'updateVINData':
            vals[0] = 'SIMULATED-VEHICLE'
        if name=='DSIGeneralVehicleStates' and method in ('updateVehicleStandstill','updateParkingBrake'):
            vals[0]=True
        if name=='DSICarTimeUnitsLanguage':
            now=datetime.now(timezone.utc)
            if method=='updateMenuLanguage': vals[0]=1
            elif method=='updateClockTime': vals[0]={'hours':now.hour,'minutes':now.minute,'seconds':now.second}
            elif method=='updateClockDate': vals[0]={'year':now.year-2000,'month':now.month,'day':now.day}
            elif method in ('updateClockViewOptions','updateUnitmasterViewOptions'):
                vals[0]={f:{'state':2} for f,t in SCHEMA['structs'][types[0][8:]] if t=='OptionalCarViewOption'}
        if name=='DSICarEco' and method=='updateStartStopViewOptions':
            vals[0]={f:{'state':2} for f,t in SCHEMA['structs']['StartStopViewOptions']}
        if (name,method) in self.values:
            vals[:-1] = self.values[name,method]
        return vals

    def handle(self, name, mid, data):
        if name=='Attributes': return self.persistence.handle(mid,data)
        if name == 'FecManager' and mid == 0:
            reader = Reader(data)
            label = decode('OptionalString', reader)
            manifest = bytes(reader.array('B'))
            signature = bytes(reader.array('B'))
            if reader.pos != len(data):
                raise ValueError('Trailing signature request data')
            valid = verify_data_signature(hashlib.sha1(manifest).digest(), signature)
            print('RCC DATA SIGNATURE', label, len(manifest), len(signature), valid, flush=True)
            return [self.reply(name, 'checkDataSignature', [label, valid])]
        d = self.definitions[name]
        signature = d['calls'].get(str(mid),'')
        method = signature.split('(')[0]
        print('DSI CALL',name,mid,signature,data[:80].hex(),flush=True)
        if name == 'DisplayPower':
            size = {0:4, 2:4, 4:5, 6:8}.get(mid)
            if size is None or len(data) != size: raise ValueError('Invalid display power request')
            display, = struct.unpack('!I', data[:4])
            brightness = mid in (0, 4)
            key = (name, display, 'brightness' if brightness else 'power')
            if mid in (4, 6):
                value, = struct.unpack('!b' if brightness else '!i', data[4:])
                if brightness and not -5 <= value <= 5: raise ValueError('Invalid brightness step')
                self.values[key] = value
            value = self.values.get(key, 0 if brightness else 1)
            return [(mid+1, struct.pack('!Ibi' if brightness else '!Iii', display, value, 0))]
        if name == 'DisplayLvds':
            if mid != 0 or len(data) != 8: raise ValueError('Invalid LVDS status request')
            status, label = struct.unpack('!iI', data)
            self.values[name, 'status'] = status
            return [(1, struct.pack('!I', label))]
        if name == 'VideoConnection':
            # Native asi.videomanagement.Connection (instance 1), recovered
            # from libasimmxvideomanagementproxy.so. These are plain int32s.
            if mid in (8, 9):
                if len(data) != 8:
                    raise ValueError('Invalid video connection request')
                terminal, label = struct.unpack('!ii', data)
                # Wire ST_ACTIVE=0, ST_STOPPED=1, ST_ERROR=2; the
                # display manager's internal connection state uses different values.
                status = (0 if mid == 9 else 1) if (terminal, label) == (0, 3) else 2
                self.values[name, terminal, label] = status
                return [(10, struct.pack('!iii', terminal, label, status))]
            if mid == 4:
                if len(data) != 4:
                    raise ValueError('Invalid video frame rate request')
                device, = struct.unpack('!i', data)
                return [(7, struct.pack('!ii', device, 10 if device == 0 else 0))]
            return []
        if method == 'setNotification':
            if 'int[]' in signature:
                if len(data)<5 or data[0] != 0: return []
                n = struct.unpack_from('!I',data,1)[0]
                if n>512 or len(data)!=5+n*4: raise ValueError('Invalid DSI subscription')
                attrs=struct.unpack_from('!'+'i'*n,data,5)
            elif 'int ' in signature: attrs=[struct.unpack('!i',data)[0]]
            else: attrs=map(int,d['attrs'])
            result=[]
            for attr in attrs:
                attrname=d['attrs'].get(str(attr),'')
                for _,spec in d['replies'].items():
                    if spec['name'].lower() == 'update'+attrname.lower():
                        result.append(self.reply(name,spec['name'],self.initial(name,spec)))
            return result
        if method in ('clearNotification','yySet','setAccessType','setGotFocus','setCarMenuState'):
            return []
        if name == 'DSIKeyPanel' and method in ('setGenericSetting', 'requestGenericSetting'):
            expected = 12 if method == 'setGenericSetting' else 8
            if len(data) != expected: raise ValueError('Invalid keypanel setting')
            panel, setting = struct.unpack('!ii', data[:8])
            key = (name, panel, setting)
            if method == 'setGenericSetting': self.values[key] = struct.unpack('!i', data[8:])[0]
            return [self.reply(name, 'genericSettingResponse', [panel, setting, self.values.get(key, 0)])]
        if name == 'DSIKeyPanel' and method == 'getProperty':
            if len(data) != 12: raise ValueError('Invalid keypanel property request')
            panel, property_id, tag = struct.unpack('!iii', data)
            # No physical panel property block is attached to this simulator.
            return [self.reply(name, 'getProperty', [panel, property_id, tag, 1, []])]
        if name == 'DSICarEco' and method in ('requestStartStopProhibitList',
                'requestStartStopRestartList', 'requestStartStopRestartProhibitList'):
            reader = Reader(data)
            info = decode('OptionalStartStopListUpdateInfo', reader)
            if reader.pos != len(data) or info is None:
                raise ValueError('Invalid start/stop list request')
            # The parked simulated vehicle has no active inhibit/restart reasons.
            info = dict(info, numOfElements=0)
            response = method.replace('request', 'response', 1).replace('List', 'ReasonList')
            return [self.reply(name, response, [info, []])]
        if name == 'DSICarComfort' and method == 'requestUGDOButtonList':
            reader = Reader(data)
            info = decode('OptionalUGDOButtonListUpdateInfo', reader)
            if info is None or reader.pos != len(data): raise ValueError('Invalid garage button list request')
            record = info['recordContent']
            suffix = str(record) if 0 <= record <= 5 else 'F' if record == 15 else None
            if suffix is None: raise ValueError('Unknown garage button record format')
            # No learned garage transmitters in the parked vehicle fixture.
            return [self.reply(name, 'responseUGDOButtonListRA'+suffix,
                               [dict(info, numOfElements=0), []])]
        if name == 'DSIDataConnection' and method == 'forceDisconnectRequest':
            if data: raise ValueError('Unexpected disconnect payload')
            return [self.reply(name, 'forceDisconnectResponse', [0])]
        if name == 'DSIDataConfiguration' and method == 'acceptDataRequest':
            if len(data) != 5: raise ValueError('Invalid data acceptance request')
            reader = Reader(data)
            application, accepted = decode('Int32', reader), decode('Bool', reader)
            self.values[name, 'acceptance', application] = accepted
            # Cancelling succeeds locally. Accepting cannot establish a data
            # session without a modem: RESULT_ERROR_CONNECTION_REFUSED=7.
            # A typed result completes the HMI request; generic asyncException
            # does not match its acceptDataRequestResponse timeout tracker.
            return [self.reply(name, 'acceptDataRequestResponse', [7 if accepted else 0])]
        if name == 'DSIDataConfiguration' and method in (
                'setRoamingState', 'setConnectionMode', 'setRequestSetting'):
            count = 2 if method == 'setRequestSetting' else 1
            if len(data) != 4*count: raise ValueError('Invalid data configuration request')
            values = list(struct.unpack('!'+'i'*count, data))
            update = 'update'+method[3:]
            self.values[name, update] = values
            return [self.reply(name, method+'Response', [0]),
                    self.reply(name, update, values+[1])]
        if name == 'DSIInfotainmentRecorder':
            if method == 'logInit':
                if data: raise ValueError('Unexpected recorder init payload')
                self.values[name, 'events'] = []
                return []
            if method in ('logPanelName', 'logKeyEvent', 'backupTrigger'):
                events = self.values.setdefault((name, 'events'), [])
                events.append((method, data.hex()))
                del events[:-128]
                return []
            if method == 'enableTrigger':
                reader = Reader(data)
                enabled, trigger = decode('Bool', reader), decode('Int32', reader)
                if reader.pos != len(data) or not 0 <= trigger < 64:
                    raise ValueError('Invalid recorder trigger')
                triggers = self.values.setdefault((name, 'updateEnabledTriggers'), [[False]*64])[0]
                triggers[trigger] = enabled
                return [self.reply(name, 'updateEnabledTriggers', [triggers, 1])]
        if name in ('DSISound', 'DSIAMFMTuner', 'DSIDABTuner') and method in (
                'profileChange', 'profileCopy', 'profileReset', 'profileResetAll'):
            count = {'profileChange': 1, 'profileCopy': 2, 'profileReset': 1, 'profileResetAll': 0}[method]
            if len(data) != 4*count: raise ValueError('Invalid profile request')
            args = list(struct.unpack('!'+'i'*count, data))
            if any(n < 0 or n > 255 for n in args): raise ValueError('Invalid profile number')
            current = self.active_profiles.get(name, 0)
            profiles = self.profiles.setdefault(name, {})
            profiles[current] = copy.deepcopy({k[1]: v for k,v in self.values.items()
                                               if len(k) == 2 and k[0] == name and k[1].startswith('update')})
            target = current
            if method == 'profileChange': target = args[0]
            elif method == 'profileCopy': profiles[args[1]] = copy.deepcopy(profiles.get(args[0], {}))
            elif method == 'profileReset': profiles[args[0]] = {}
            else: profiles.clear()
            for k in list(self.values):
                if len(k) == 2 and k[0] == name and k[1].startswith('update'): del self.values[k]
            for attr, value in profiles.get(target, {}).items(): self.values[name, attr] = copy.deepcopy(value)
            self.active_profiles[name] = target
            response = {'profileChange':'profileChanged', 'profileCopy':'profileCopied'}.get(method, method)
            return [self.reply(name, response, args+[0]),
                    self.reply(name, 'updateProfileState', [2, target, 1])]
        if name == 'DSIAudioManagement':
            if method in ('requestConnection', 'fadeToConnection', 'releaseConnection',
                          'getActiveConnection', 'getActiveEntertainmentConnection'):
                expected = 3 if method == 'requestConnection' else (1 if method.startswith('get') else 2)
                if len(data) != expected*4:
                    raise ValueError('Invalid audio connection request')
                args = struct.unpack('!'+'i'*expected, data)
                if method.startswith('get'):
                    terminal = args[0]
                    active = self.values.get(('audio', terminal), 0)
                    return [self.reply(name, 'update'+method[3:], [active, terminal, 1])]
                connection, terminal = args[:2]
                active = self.values.get(('audio', terminal), 0)
                if method == 'releaseConnection':
                    if active == connection:
                        self.values['audio', terminal] = 0
                    return [self.reply(name, 'stopConnection', [connection, terminal]),
                            self.reply(name, 'updateActiveConnection', [self.values.get(('audio', terminal), 0), terminal, 1])]
                if method == 'requestConnection':
                    return [self.reply(name, 'startConnection', [connection, terminal])]
                self.values['audio', terminal] = connection
                return [self.reply(name, 'updateActiveConnection', [connection, terminal, 1]),
                        self.reply(name, 'fadedIn', [connection, terminal])]
            if method in ('setVolumelock', 'getVolumelock'):
                if len(data) != (9 if method.startswith('set') else 8):
                    raise ValueError('Invalid volume lock request')
                terminal, connection = struct.unpack('!ii', data[:8])
                key = ('audio-lock', terminal, connection)
                if method.startswith('set'):
                    self.values[key] = bool(data[8])
                return [self.reply(name, 'responseVolumelock', [terminal, connection, self.values.get(key, False)])]
        if name=='DSIKOMOGfxStreamSink':
            if method=='setFGLayer':
                layer=struct.unpack('!i',data)[0]
                if not 0<=layer<=3:raise ValueError('Invalid cluster layer')
                self.values[name,'layer']=layer
                return [self.reply(name,'setFGLayerResult',[layer])]
            if method=='fadeIn':
                if len(data)!=12:raise ValueError('Invalid fade parameters')
                self.values[name,'fade']=struct.unpack('!iii',data)
                return [self.reply(name,'fadeInResult',[]),self.reply(name,'updateRequestSync',[0,1])]
            if method=='fadeOut':
                self.values[name,'fade']=struct.unpack('!i',data)
                return [self.reply(name,'fadeOutResult',[])]
        if name=='DSIKOMONavInfo' and method.startswith('set'):
            self.values[name,method]=bytes(data)
            result=method+'Result'
            spec=next((v for v in d['replies'].values() if v['name']==result),None)
            if spec and spec['types']==['Int32']:return [self.reply(name,result,[0])]
            if method in ('setDistanceToNextManeuver','setDistanceToDestination','setETA','setRTT','setMapScaleResult'):return []
        values=None
        if name == 'DSISwdlDeviceInfo':
            a=struct.unpack('!'+'i'*(len(data)//4),data) if data else ()
            device=self.inventory[a[0]] if a and 0<=a[0]<len(self.inventory) else {'modules':[]}
            modules=device['modules']
            module=modules[a[1]] if len(a)>1 and 0<=a[1]<len(modules) else {'version':0}
            if method=='getDevices': values=[[d['name'] for d in self.inventory],[0]*len(self.inventory)]
            elif method=='getModules': values=[a[0],[m['name'] for m in modules],[0]*len(modules),[m['hw'] for m in modules]]
            elif method in ('getVersions','getTargetVersions'): values=[*a,[module['version']]]
            elif method in ('isDataModule','isNoExclusiveBoloUpdate'): values=[*a,False]
            elif method=='getAdditionalInfo': values=[*a,[0]]
            elif method=='getLanguages': values=[a[0],['en_GB'],0,0,0]
            elif method=='getErrors': values=[a[0],[],[]]
            elif method=='getFileNames': values=[*a,[]]
            elif method=='getInfoFilePath': values=[a[0],'','']
            elif method=='getNumberOfPopups': values=[0]
        elif name=='DSISwdlLogging' and method=='getHistory': values=[[],[]]
        elif name=='DSISwdlSelection':
            if method=='getMedia': values=[[]]
            elif method=='getUserDefinedAllowed': values=[False]
            elif method=='getIncompatibleDevices': values=[[],[]]
            elif method=='getFinalizeTargets': values=[[]]
            elif method in ('abortSetMedium','abortSetRelease'): values=[]
        if values is not None: return [self.reply(name,method,values)]
        if method.startswith('set'):
            update='update'+method[3:]
            spec=next((v for v in d['replies'].values() if v['name']==update),None)
            if spec is not None:
                reader=Reader(data)
                values=[decode(t,reader) for t in spec['types'][:-1]]
                if reader.pos!=len(data):raise ValueError('Trailing DSI setter data')
                self.values[name,update]=values
                return [self.reply(name,update,values+[1])]
        # Explicitly reject unimplemented requests instead of claiming success.
        if method and any(v['name']=='asyncException' for v in d['replies'].values()):
            # HMI timeout tracking uses RT_* identifiers, not proxy wire IDs.
            request_id = d.get('request_ids', {}).get(str(mid), mid)
            return [self.reply(name,'asyncException',[1,'Not supported by simulated vehicle',request_id])]
        return []

#!/usr/bin/env python3
"""Experimental RCC Ethernet peer. Captures traffic and accepts ESO TCP links.

Uses framed Ethernet over FIFOs, or an optional inherited stream socket.
No guest binaries or HMI event handlers are replaced by this peer.
"""
import argparse
import atexit
import signal
import sys
import os
import socket
import struct
import time
import select
import uuid
import json
import stat
from firmware_profile import current
from rcc_services import Services
from rcc_features import state_vector
from audio_host import AudioEndpoint
from pathlib import Path

MAC = bytes.fromhex('000102030405')
IP = socket.inet_aton('10.0.0.16')
AGENT_UUID = uuid.UUID('78a07e56-255a-4309-8bde-b1e1c73bc71c').bytes
AGENT_KEY = uuid.UUID('299585a3-5e89-5854-a716-dafd54af2e50').bytes
KEYPANEL_UUID = uuid.UUID('beff9a63-a8c2-503f-8909-dd7157fda6dc').bytes
KEYPANEL_KEY = uuid.UUID('50130e55-eb52-532e-ab5c-62781c7f5705').bytes
WIRE_TRACE = os.environ.get('MHI2_RCC_TRACE') == '1'

def instance(uid, handle, interface_key):
    # IDL OptionalInstanceID -> OptionalUUID -> OptionalUInt8VarArray.
    return b'\0\0\0' + struct.pack('!I', 16) + uid + struct.pack('!I', handle) + b'\0\0' + struct.pack('!I', 16) + interface_key

def checksum(data):
    data += bytes(len(data) & 1)
    total = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535

class Peer:
    def __init__(self, sock, capture):
        self.sock, self.capture = sock, capture
        self.flows = {}
        self.cluster_requested = False
        self.services = Services()
        self.feature_states = state_vector()  # Configuration applies at startup.
        self.profile = current()
        self.next_connect = time.monotonic() + 5
        self.local_port = 40000 + int(time.monotonic()) % 20000
        self.audio = AudioEndpoint()
        self.guest_mac = bytes.fromhex('000504030201')
        self.epoch = int(time.time()) & 0x7fff
        self.capture.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))

    def record(self, frame):
        now = time.time()
        self.capture.write(struct.pack('<IIII', int(now), int(now % 1 * 1000000), len(frame), len(frame)) + frame)
        if WIRE_TRACE:
            self.capture.flush()

    def send(self, frame):
        self.record(frame)
        self.sock.sendall(struct.pack('!I', len(frame)) + frame)

    def ip(self, mac, target, protocol, data):
        header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(data), 0, 0x4000, 64, protocol, 0, IP, target)
        header = header[:10] + struct.pack('!H', checksum(header)) + header[12:]
        self.send(mac + MAC + b'\x08\x00' + header + data)

    def send_segment(self, key, flow, seq, flags, data):
        target, remote, local = key
        header = struct.pack('!HHIIBBHHH', local, remote, seq, flow['ack'], 0x50, flags, 32768, 0, 0)
        pseudo = IP + target + struct.pack('!BBH', 0, 6, len(header) + len(data))
        header = header[:16] + struct.pack('!H', checksum(pseudo + header + data)) + header[18:]
        self.ip(flow['mac'], target, 6, header + data)

    def flush_tcp(self, key, flow):
        now = time.monotonic()
        pending = flow.setdefault('pending', [])
        for segment in pending[:8]:
            if segment['sent'] is None:
                self.send_segment(key, flow, segment['seq'], segment['flags'], segment['data'])
                segment['sent'] = now
        if pending and now - pending[0]['sent'] > 1:
            first = pending[0]
            self.send_segment(key, flow, first['seq'], first['flags'], first['data'])
            first['sent'] = now

    def tcp(self, key, flow, flags=0x10, data=b''):
        if data and len(data)>1400:
            for offset in range(0,len(data),1400):
                self.tcp(key,flow,flags,data[offset:offset+1400])
            return
        consumed = len(data) + bool(flags & 2) + bool(flags & 1)
        if consumed:
            pending=flow.setdefault('pending', [])
            if sum(len(s['data']) for s in pending)+len(data)>1024*1024:
                raise ValueError('TCP send queue exceeded 1 MiB')
            pending.append({'seq':flow['seq'], 'end':(flow['seq']+consumed)&0xffffffff,
                            'flags':flags,'data':data,'sent':None})
            flow['seq']=(flow['seq']+consumed)&0xffffffff
            self.flush_tcp(key,flow)
        else:
            self.send_segment(key,flow,flow['seq'],flags,data)

    def comm(self, key, flow, message):
        if WIRE_TRACE: print('ESO SEND', key[1:], message.hex(), flush=True)
        self.tcp(key, flow, 0x18, struct.pack('!I', len(message)) + message)

    def register_services(self, key, flow):
        registrations = [('DSIKeyPanel', KEYPANEL_UUID, KEYPANEL_KEY, 0)]
        registrations.extend(self.services.registrations())
        for name, uid, interface_key, handle in registrations:
            payload = instance(uid, handle, interface_key) + struct.pack('!H',402)
            self.comm(key, flow, b'\x06' + struct.pack('!HH',flow['broker_stub'],2) + payload)
            print('Registered',name,flush=True)

    def input_event(self, event):
        if event.get('type') in ('cluster', 'cluster-map', 'cluster-setup'):
            if event['type'] == 'cluster':
                self.cluster_requested = True
            # Native DSI provider is separate from the Java HMI (agent 200).
            map_view = event['type'] == 'cluster-map'
            map_instance = int(event.get('instance', 3))
            if map_view and map_instance not in (0, 3):
                raise ValueError('Only native main and cluster map instances are supported')
            map_position = None
            if map_view and ('latitude' in event or 'longitude' in event):
                lat, lon = float(event['latitude']), float(event['longitude'])
                if not -90 <= lat <= 90 or not -180 <= lon <= 180:
                    raise ValueError('Invalid map coordinates')
                # Native NaviHelper.decimalToDsi scale and serializer order.
                map_position = (int(lon * 11930464), int(lat * 11930464))
            client = 'mapviewer' if map_view else 'displaymanagement'
            port = 21000 + int(event.get('agent', 322 if map_view else 103))
            for old_key, old_flow in list(self.flows.items()):
                if (old_flow.get('client') == client and old_key[1] == port and
                        (not map_view or old_flow['map_instance'] == map_instance)):
                    old_flow['map_position'] = map_position
                    old_flow['cluster_context'] = int(event.get('context', 70))
                    old_flow['setup_only'] = event['type'] == 'cluster-setup'
                    if 'native_stub' in old_flow:
                        self.request_native_view(old_key, old_flow)
                    elif (not old_flow.get('connected') and
                          time.monotonic() - old_flow.get('created', 0) > 10):
                        # Retry an unestablished transport, but keep an accepted
                        # service request pending while native DSI initializes.
                        self.tcp(old_key, old_flow, 0x14)
                        self.flows.pop(old_key)
                        break
                    return
            self.local_port+=1
            key=(socket.inet_aton('10.0.0.15'),21000+int(event.get('agent',322 if map_view else 103)),self.local_port)
            flow={'seq':0x310000+self.local_port*1000,'ack':0,
                  'mac':self.guest_mac,'data':bytearray(),'active':True,
                  'connected':False,'client':'mapviewer' if map_view else 'displaymanagement',
                  'created':time.monotonic(),
                  'setup_only':event['type'] == 'cluster-setup',
                  'map_instance':map_instance,'map_position':map_position,
                  'cluster_context':int(event.get('context',70)),'local_agent':499}
            self.flows[key]=flow;self.tcp(key,flow,2)
            return
        x, y = int(event.get('x', 0)), int(event.get('y', 0))
        if not 0 <= x < 800 or not 0 <= y < 480:
            raise ValueError('Touch coordinates outside 800x480 display')
        for key, flow in self.flows.items():
            if 'keypanel_reply' not in flow: continue
            now = int(time.monotonic() * 1000) & 0x7fffffff
            if event['type'] == 'touch':
                if not self.profile['touch']:
                    raise ValueError('This firmware uses the console controller, not a touchscreen')
                gesture = {'press': 4, 'release': 3, 'tap': 1, 'drag': 5}[event['action']]
                payload = struct.pack('!iiiBiiiiii', 13, gesture, 1, 0, x, y, 1, 0, now, 1)
                method = 42
            elif event['type'] == 'key':
                if not 0 <= int(event['code']) <= 116 or int(event['pressed']) not in (0, 1):
                    raise ValueError('Invalid key code or state')
                # VW uses the display panel (ABT 13); Audi uses the
                # center-console controller (FCC 1).
                payload = struct.pack('!iiiii', self.profile['keyboard'], int(event['code']), int(event['pressed']), now, 1)
                method = 38
            elif event['type'] == 'encoder':
                code, ticks = int(event['code']), int(event['ticks'])
                if code not in (16, 17) or not -32 <= ticks <= 32 or not ticks:
                    raise ValueError('Invalid encoder or tick count')
                payload = struct.pack('!iiiii', self.profile['keyboard'], code, ticks, 0, 1)
                method = 30
            else: raise ValueError('Unknown input type')
            self.comm(key, flow, b'\x06' + struct.pack('!HH', flow['keypanel_reply'], method) + payload)
            print('Production input', event, flush=True)

    def connect_native_view(self, key, flow):
        map_view = flow['client'] == 'mapviewer'
        request = (b'\x0c'+struct.pack('!HH', flow['local_agent'], 80)+
                   uuid.UUID('3a421f5d-d9bc-5bdc-b07f-07d896eb7490' if map_view else '0d85c3ca-aa14-5938-88df-8dcafa49fe81').bytes+
                   struct.pack('!I', flow['map_instance'] if map_view else 0)+
                   uuid.UUID('e4e90991-b4f4-5a57-a32f-5a4c8fcc1f67' if map_view else 'f20ac96e-5798-5645-b002-79c3f154460f').bytes+
                   struct.pack('!IH', (flow['local_agent'] << 16) | 81, 81))
        self.comm(key, flow, request)
        flow['service_requested'] = time.monotonic()

    def request_native_view(self, key, flow, subscribe=False):
        stub = flow['native_stub']
        def call(mid, payload):
            self.comm(key, flow, b'\x06' + struct.pack('!HH', stub, mid) + payload)
        if flow['client'] == 'mapviewer':
            if subscribe:
                attrs = (1, 7, 9, 13, 15, 18)
                call(69, struct.pack('!BI6i', 0, len(attrs), *attrs))
            if not flow.get('map_ready'):
                return
            if flow['map_position'] is not None:
                call(56, struct.pack('!Bii', 0, *flow['map_position']))
            call(148, b'\xff')
            print('NATIVE CLUSTER: requested map viewer', flow['map_instance'], 'visibility', flush=True)
        else:
            requests = [(55, (4, 2)), (24, (flow['cluster_context'], 4, 0))]
            map_visible = any(f.get('client') == 'mapviewer' and
                              f.get('map_instance') == 3 and f.get('map_visible')
                              for f in self.flows.values())
            if self.cluster_requested and map_visible:
                requests += [(57, (4, 10))]
            for mid, values in requests:
                call(mid, struct.pack('!' + 'i' * len(values), *values))
            print('NATIVE CLUSTER: requested H.264 display 4, context '+str(flow['cluster_context'])
                  if self.cluster_requested and map_visible else
                  'NATIVE CLUSTER: configured display 4; video waits for native map visibility', flush=True)

    def tick(self):
        self.audio.tick()
        now = time.monotonic()
        for key, flow in list(self.flows.items()):
            self.flush_tcp(key,flow)
            if (flow.get('client') in ('mapviewer', 'displaymanagement') and
                    'service_requested' in flow and 'native_stub' not in flow and
                    now - flow['service_requested'] >= 5):
                self.connect_native_view(key, flow)
        if now < self.next_connect: return
        self.next_connect = now + 5
        # The physical RCC publishes its FEC state vector to MMX agent 326.
        # Keep the native MMX distributor and all feature consumers running.
        feature_flows = [(k, f) for k, f in self.flows.items() if f.get('client') == 'features']
        for k, f in feature_flows:
            if 'native_stub' not in f and now-f['created'] > 10:
                self.tcp(k, f, 0x14)
                self.flows.pop(k)
        if not any(f.get('client') == 'features' for f in self.flows.values()):
            self.local_port += 1
            k = (socket.inet_aton('10.0.0.15'), 21326, self.local_port)
            f = {'seq': 0x310000+self.local_port*1000, 'ack': 0,
                 'mac': self.guest_mac, 'data': bytearray(), 'active': True,
                 'connected': False, 'client': 'features', 'created': now,
                 'local_agent': 499}
            self.flows[k] = f
            self.tcp(k, f, 2)
        for key, flow in list(self.flows.items()):
            if flow.get('active') and not flow.get('connected') and flow.get('client') != 'features':
                self.flows.pop(key)
        # The attached MOST sink is H.264 capable before the HMI's first rate
        # request. Configure its display type and native map context as soon
        # as the DSI starts; rate and map visibility stay with the HMI/viewer.
        if not any(f.get('client') == 'displaymanagement' and 'native_stub' in f
                   for f in self.flows.values()):
            self.input_event({'type': 'cluster-setup'})
        if any(flow.get('active') and not flow.get('client') for flow in self.flows.values()): return
        self.local_port = 40000 + (self.local_port + 1 - 40000) % 20000
        key = socket.inet_aton('10.0.0.15'), 21100, self.local_port
        flow = {'seq': 0x210000 + self.local_port * 1000, 'ack': 0,
                'mac': self.guest_mac, 'data': bytearray(), 'active': True,
                'connected': False}
        self.flows[key] = flow
        self.tcp(key, flow, 2)
        print('Connecting RCC agent 402 to MMX broker', flush=True)

    def messages(self, key, flow):
        buf = flow['data']
        while len(buf) >= 4:
            size = struct.unpack_from('!I', buf)[0]
            if size > 1024 * 1024: raise ValueError('Oversize ESO message')
            if len(buf) < size + 4: break
            msg = bytes(buf[4:4 + size])
            del buf[:4 + size]
            if not msg: continue
            if WIRE_TRACE: print('ESO RECV', key[1:], msg[:256].hex(), flush=True)
            kind = msg[0]
            if kind == 1 and flow.get('active'):
                self.comm(key, flow, bytes([2, msg[3]]))
                if flow.get('client') == 'features':
                    self.comm(key, flow, b'\x03'+struct.pack('!HH', 499, 80)+
                              uuid.UUID('ff733e4d-6e39-49b8-b354-ee9c22f6cfc9').bytes+
                              struct.pack('!I', 1))
                if flow.get('client') in ('displaymanagement','mapviewer'):
                    self.connect_native_view(key, flow)
            elif kind==13 and flow.get('client')=='mapviewer':
                proxy,stub,reply_proxy=struct.unpack_from('!HHH',msg,1)
                flow['native_stub']=stub
                self.comm(key,flow,b'\x07'+struct.pack('!H',stub))
                # Diagnostic request to the firmware's own instance-3 renderer;
                # no HMI framebuffer substitution or synthetic map content.
                self.request_native_view(key,flow,subscribe=True)
            elif kind==6 and flow.get('client')=='mapviewer':
                # Native DSIMapViewerControlReply: updateReady (126) and
                # updateViewVisible (142), Bool + validity flag (low 7 bits: 1 = valid; bit 7: changed).
                if len(msg) == 10:
                    mid = struct.unpack_from('!H', msg, 3)[0]
                    valid = (struct.unpack_from('!i', msg, 6)[0] & 0x7f) == 1
                    if mid == 126:
                        flow['map_ready'] = bool(msg[5]) and valid
                        if flow['map_ready']:
                            self.request_native_view(key, flow)
                    elif mid == 142:
                        flow['map_visible'] = bool(msg[5]) and valid
                        if flow['map_visible'] and flow['map_instance'] == 3:
                            for dk, df in list(self.flows.items()):
                                if df.get('client') == 'displaymanagement' and 'native_stub' in df:
                                    self.request_native_view(dk, df)
                print('NATIVE CLUSTER MAP reply',msg.hex(),flush=True)
            elif kind==13 and flow.get('client')=='displaymanagement':
                proxy,stub,reply_proxy=struct.unpack_from('!HHH',msg,1)
                flow['display_stub']=stub
                flow['native_stub']=stub
                self.comm(key,flow,b'\x07'+struct.pack('!H',stub))
                self.request_native_view(key,flow)
            elif kind==6 and flow.get('client')=='displaymanagement':
                print('NATIVE CLUSTER reply',msg.hex(),flush=True)
            elif kind == 9 and flow.get('active'):
                # BrokerAck V4 supplies the broker UUID/handle and interface key.
                broker = msg[3:23]
                self.comm(key, flow, b'\x03' + struct.pack('!HH', 402, 1) + broker)
            elif kind == 0 and len(msg) >= 6:
                own_id = key[2] - 21000
                flow['agent'] = struct.unpack_from('!H', msg, 2)[0]
                self.comm(key, flow, b'\x01' + struct.pack('!HBH', own_id, 1, self.epoch))
            elif kind == 4 and flow.get('client') == 'features':
                proxy, stub = struct.unpack_from('!HH', msg, 1)
                flow['native_stub'] = stub
                self.comm(key, flow, b'\x07'+struct.pack('!H', stub))
                self.comm(key, flow, b'\x06'+struct.pack('!HH', stub, 1)+self.feature_states)
                print('SIMULATED RCC: feature states delivered to native MMX FEC service', flush=True)
            elif kind == 4 and flow.get('active'):
                proxy, stub = struct.unpack_from('!HH', msg, 1)
                flow['broker_stub'] = stub
                self.comm(key, flow, b'\x07' + struct.pack('!H', stub))
                self.comm(key, flow, b'\x06' + struct.pack('!HH', stub, 0) + instance(AGENT_UUID, 402, AGENT_KEY))
                self.register_services(key, flow)
            elif kind == 3 and msg[5:21] == AGENT_UUID:
                agent, proxy = struct.unpack_from('!HH', msg, 1)
                flow['agent_stub'] = 2
                self.comm(key, flow, b'\x04' + struct.pack('!HH', proxy, 2))
            elif kind == 7 and flow.get('active') and not flow.get('registered'):
                stub = struct.unpack_from('!H', msg, 1)[0]
                if stub == flow.get('agent_stub'):
                    flow['registered'] = True
                    self.register_services(key, flow)
                    print('Registered production DSIKeyPanel[0] on RCC agent 402', flush=True)
            elif kind == 12 and msg[5:21] in self.services.by_uuid:
                if len(msg) < 47: continue
                name = self.services.by_uuid[msg[5:21]]
                agent, proxy = struct.unpack_from('!HH', msg, 1)
                reply_stub = struct.unpack_from('!H', msg, 45)[0]
                channels = flow.setdefault('services', {})
                stub = 300 + len(channels)
                channels[stub] = (name, reply_stub)
                self.comm(key, flow, b'\x0d' + struct.pack('!HHH', proxy, stub, stub+1000))
                self.comm(key, flow, b'\x07' + struct.pack('!H', reply_stub))
                print('DSI CONNECT',name,stub,reply_stub,flush=True)
            elif kind == 6 and len(msg) >= 5 and struct.unpack_from('!H',msg,1)[0] in flow.get('services', {}):
                stub, method = struct.unpack_from('!HH', msg, 1)
                name, reply_stub = flow['services'][stub]
                try:
                    for mid, payload in self.services.handle(name,method,msg[5:]):
                        self.comm(key, flow, b'\x06' + struct.pack('!HH',reply_stub,mid) + payload)
                except (ValueError, KeyError, struct.error) as exc:
                    print('DSI request rejected',name,method,repr(exc),flush=True)
            elif kind == 12 and msg[5:21] == KEYPANEL_UUID:
                agent, proxy = struct.unpack_from('!HH', msg, 1)
                reply_stub = struct.unpack_from('!H', msg, 45)[0]
                flow['keypanel_reply'] = reply_stub
                self.comm(key, flow, b'\x0d' + struct.pack('!HHH', proxy, 100, 200))
                self.comm(key, flow, b'\x07' + struct.pack('!H', reply_stub))
                print('Production keypanel reply channel', reply_stub, flush=True)
            elif kind == 6 and len(msg) >= 5 and flow.get('keypanel_reply'):
                stub, method = struct.unpack_from('!HH', msg, 1)
                if stub == 100 and method == 12 and len(msg) >= 10:
                    count = struct.unpack_from('!I', msg, 6)[0]
                    if count > 64 or len(msg) != 10 + count * 4:
                        print('Invalid keypanel subscription array', flush=True)
                        continue
                    attrs = struct.unpack_from('!' + 'I' * count, msg, 10)
                    print('Keypanel subscriptions', attrs, flush=True)
                    for attr in attrs:
                        initial = {25: (38, (self.profile['keyboard'], 0, 0, 0, 129)),
                                   26: (43, (self.profile['keyboard'], 1, 129)),
                                   19: (6, (1, 129)),
                                   23: (30, (self.profile['keyboard'], 0, 0, 0, 129))}.get(attr)
                        if initial:
                            mid, values = initial
                            self.comm(key, flow, b'\x06' + struct.pack('!HH', flow['keypanel_reply'], mid) + struct.pack('!' + 'i' * len(values), *values))
                        elif attr == 20:
                            self.comm(key, flow, b'\x06' + struct.pack('!HHiiiBiiiiii', flow['keypanel_reply'], 42, self.profile['keyboard'], 0, 1, 0, 0, 0, 0, 0, 0, 129))
                elif stub == 100:
                    try:
                        for mid, payload in self.services.handle('DSIKeyPanel', method, msg[5:]):
                            self.comm(key, flow, b'\x06' + struct.pack('!HH', flow['keypanel_reply'], mid) + payload)
                    except (ValueError, KeyError, struct.error) as exc:
                        print('Keypanel request rejected', method, repr(exc), flush=True)
            elif kind == 14:
                print('ESO ping', msg.hex(), flush=True)

    def receive(self, frame):
        self.record(frame)
        if len(frame) < 14: return
        src, kind, payload = frame[6:12], frame[12:14], frame[14:]
        if kind == b'\x08\x06' and len(payload) >= 28:
            if payload[:8] == bytes.fromhex('0001080006040001') and payload[24:28] == IP:
                print('ARP request for RCC', flush=True)
                self.send(src + MAC + kind + bytes.fromhex('0001080006040002') + MAC + IP + src + payload[14:18])
        elif kind == b'\x08\x00' and len(payload) >= 20:
            ihl = (payload[0] & 15) * 4
            total = struct.unpack_from('!H', payload, 2)[0]
            if ihl < 20 or total > len(payload) or payload[16:20] != IP: return
            remote, protocol, body = payload[12:16], payload[9], payload[ihl:total]
            if protocol == 1 and len(body) >= 8 and body[0] == 8:
                reply = b'\x00\x00\x00\x00' + body[4:]
                reply = reply[:2] + struct.pack('!H', checksum(reply)) + reply[4:]
                self.ip(src, remote, 1, reply)
                print('ICMP echo replied', flush=True)
            elif protocol == 17 and len(body) >= 8:
                sport, dport, length, udp_checksum = struct.unpack_from('!HHHH', body)
                if dport == 50000 and 8 <= length <= len(body):
                    reply = self.audio.process(body[8:length])
                    if reply is not None:
                        udp = struct.pack('!HHHH', dport, sport, len(reply)+8, 0) + reply
                        self.ip(src, remote, 17, udp)
            elif protocol == 6 and len(body) >= 20:
                sport, dport, seq, ack = struct.unpack_from('!HHII', body)
                offset, flags = (body[12] >> 4) * 4, body[13]
                if offset < 20 or offset > len(body): return
                key = remote, sport, dport
                if key in self.flows and flags & 0x10:
                    flow = self.flows[key]
                    flow['pending'] = [s for s in flow.get('pending',[]) if ((ack-s['end'])&0xffffffff)>=0x80000000]
                    self.flush_tcp(key,flow)
                if flags & 4:
                    self.flows.pop(key, None)
                    return
                if flags & 2 and key in self.flows and self.flows[key].get('active'):
                    flow = self.flows[key]
                    flow['ack'] = (seq + 1) & 0xffffffff
                    if ack != flow['seq']: return
                    self.tcp(key, flow)
                    if not flow['connected']:
                        flow['connected'] = True
                        self.comm(key, flow, b'\x00' + struct.pack('!BHH', 5, flow.get('local_agent',402), self.epoch))
                elif flags & 2 and key in self.flows:
                    flow = self.flows[key]
                    self.send_segment(key,flow,(flow['seq']-1)&0xffffffff,0x12,b'')
                elif flags & 2:
                    flow = {'seq': 0x100000 + len(self.flows) * 0x10000, 'ack': (seq + 1) & 0xffffffff, 'mac': src, 'data': bytearray()}
                    self.flows[key] = flow
                    self.tcp(key, flow, 0x12)
                    print('TCP SYN', sport, '->', dport, flush=True)
                elif key in self.flows:
                    flow = self.flows[key]
                    data = body[offset:]
                    if seq == flow['ack']:
                        flow['ack'] = (seq + len(data) + bool(flags & 1)) & 0xffffffff
                        if data:
                            flow['data'].extend(data)
                            if WIRE_TRACE: print('TCP DATA', sport, '->', dport, len(data), data[:128].hex(), flush=True)
                    if data or flags & 1: self.tcp(key, flow)
                    if data: self.messages(key, flow)
                    if flags & 1:
                        self.tcp(key, flow, 0x11)
                        self.flows.pop(key, None)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fd', type=int)
    parser.add_argument('--capture', default='/tmp/mhi2-rcc.pcap')
    parser.add_argument('--reload', action='store_true')
    args = parser.parse_args()
    control_path = '/tmp/mhi2-rcc-input'
    if not os.path.exists(control_path): os.mkfifo(control_path, 0o600)
    if not stat.S_ISFIFO(os.stat(control_path).st_mode): raise ValueError('Input path is not a FIFO')
    control = os.open(control_path, os.O_RDWR | os.O_NONBLOCK)
    controls = bytearray()
    class PipeTransport:
        def recv(self, size): return os.read(0, size)
        def sendall(self, data):
            while data:
                size = os.write(1, data)
                data = data[size:]
    sock = PipeTransport() if args.fd is None else socket.socket(socket.AF_UNIX, socket.SOCK_STREAM, 0, fileno=args.fd)
    # stdout carries Ethernet frames in pipe mode.
    if args.fd is None:
        sys.stdout = sys.stderr
    mtime = Path(__file__).stat().st_mtime_ns
    with open(args.capture, 'wb') as capture:
        peer = Peer(sock, capture)
        atexit.register(peer.audio.close)
        signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(0))
        buffer = bytearray()
        while True:
            peer.tick()
            try:
                controls.extend(os.read(control, 4096))
                if len(controls) > 16384: controls.clear()
                while b'\n' in controls:
                    line, _, controls = controls.partition(b'\n')
                    try: peer.input_event(json.loads(line))
                    except (ValueError, KeyError, TypeError) as exc: print('Invalid input:', exc, flush=True)
            except BlockingIOError: pass
            if args.reload and Path(__file__).stat().st_mtime_ns != mtime:
                for key, flow in list(peer.flows.items()): peer.tcp(key, flow, 0x14)
                peer.audio.close()
                capture.close()
                Path(args.capture).rename(args.capture + '.' + time.strftime('%H%M%S'))
                os.execv(sys.executable, [sys.executable, __file__, '--reload', '--capture', args.capture])
            transport_fd = args.fd if args.fd is not None else 0
            ready, _, _ = select.select([transport_fd, control], [], [], 0.1)
            # Wake immediately for a key/touch event; consume it at the top of
            # the loop without blocking on unrelated Ethernet input.
            if transport_fd not in ready: continue
            data = sock.recv(65536)
            if not data: break
            buffer.extend(data)
            while len(buffer) >= 4:
                length = struct.unpack_from('!I', buffer)[0]
                if length > 65535:
                    raise ValueError(f'Invalid Ethernet frame size {length}; prefix={buffer[:32].hex()}')
                if len(buffer) < length + 4: break
                frame = bytes(buffer[4:4 + length])
                del buffer[:4 + length]
                peer.receive(frame)

if __name__ == '__main__': main()

"""Explicit clients of the firmware's network services, never replacements."""
import socket
import struct
import time
import uuid
from rcc_services import decode, encode
from rcc_persistence import Reader

WLAN_UUID = 'cb957d6d-4fc4-57ce-9169-f70096df25aa'
WLAN_KEY = '99c7079e-21cf-55ef-9908-8802a5f2a7d7'
DATA_UUID = '6f242ae7-d62e-555f-a51f-1b658fcd1390'
DATA_KEY = 'dd69ea9b-1ce5-5e11-a4ed-663f5d66eb9a'


def start(peer, event):
    action = event.get('action')
    if action not in ('wifi', 'cellular'):
        raise ValueError('Network action must be wifi or cellular')
    from firmware_profile import common_metadata
    if common_metadata().get('release') != 'MHI2_ER_POG11_K5126':
        raise ValueError('Native network client currently supports POG11 K5126')
    ssid = event.get('ssid', 'QEMU Wi-Fi')
    if (not isinstance(ssid, str) or not 1 <= len(ssid.encode()) <= 32 or
            '\0' in ssid):
        raise ValueError('Invalid SSID')
    for old_key, old_flow in list(peer.flows.items()):
        if old_flow.get('client') == 'network' and old_flow.get('action') == action:
            peer.tcp(old_key, old_flow, 0x14)
            del peer.flows[old_key]
    peer.local_port += 1
    key = (socket.inet_aton('10.0.0.15'), 21316, peer.local_port)
    flow = dict(seq=0x310000 + peer.local_port * 1000, ack=0,
                mac=peer.guest_mac, data=bytearray(), active=True,
                connected=False, client='network', created=time.monotonic(),
                local_agent=498 if action == 'wifi' else 497,
                ssid=ssid, action=action)
    peer.flows[key] = flow
    peer.tcp(key, flow, 2)


def message(peer, key, flow, msg):
    def call(mid, data=b''):
        peer.comm(key, flow, b'\x06' + struct.pack('!HH', flow['native_stub'], mid) + data)
    if msg[0] == 1 and len(msg) >= 4:
        peer.comm(key, flow, bytes([2, msg[3]]))
        service, interface_key = ((WLAN_UUID, WLAN_KEY) if flow['action'] == 'wifi'
                           else (DATA_UUID, DATA_KEY))
        agent = flow['local_agent']
        peer.comm(key, flow, b'\x0c' + struct.pack('!HH', agent, 80) +
                  uuid.UUID(service).bytes + struct.pack('!I', 0) +
                  uuid.UUID(interface_key).bytes + struct.pack('!IH', (agent << 16) | 81, 81))
    elif msg[0] == 13 and len(msg) >= 7:
        _, stub, _ = struct.unpack_from('!HHH', msg, 1)
        flow['native_stub'] = stub
        peer.comm(key, flow, b'\x07' + struct.pack('!H', stub))
        if flow['action'] == 'wifi':
            call(12)
            call(19, struct.pack('!i', 2))  # Infrastructure station.
        else:
            call(18)
            call(36, encode('OptionalCDataProfile', dict(
                profileID=0, dataProfileName='QEMU', dataAPN='qemu',
                provider='QEMU', isAPNvisible=True)))
            call(14, struct.pack('!i', 0))  # Automatic connection.
    elif msg[0] == 6 and len(msg) >= 5:
        mid = struct.unpack_from('!H', msg, 3)[0]
        # Profiles can include passwords: never dump arbitrary reply bytes.
        print('NATIVE NETWORK', flow['action'], 'reply', mid, flush=True)
        if flow['action'] == 'cellular':
            if mid == 28 and len(msg) == 13:
                request, validity = struct.unpack_from('!ii', msg, 5)
                if request >= 0 and validity & 0x7f == 1:
                    call(0, struct.pack('!iB', request, 255))
            elif mid in (1, 15, 22, 24) and len(msg) == 9:
                print('NATIVE NETWORK result', mid,
                      struct.unpack_from('!i', msg, 5)[0], flush=True)
            return
        if mid in (53, 54) and len(msg) == 9:
            print('NATIVE NETWORK wifi result', mid,
                  struct.unpack_from('!i', msg, 5)[0], flush=True)
        if mid == 47:
            reader = Reader(msg[5:])
            try:
                decode('OptionalString', reader)
                decode('OptionalString', reader)
                result = decode('Int32', reader)
            except ValueError:
                print('NATIVE NETWORK malformed wifi connection reply', flush=True)
                return
            print('NATIVE NETWORK wifi connection result', result, flush=True)
        elif mid == 54 and msg[5:9] == bytes(4):
            call(17, b'\xff')
        elif mid == 53 and msg[5:9] == bytes(4):
            call(45, struct.pack('!ii', 0, 0))
        elif mid == 51 and len(msg) == 13 and msg[9:13] == bytes(4):
            call(59, encode('OptionalString', flow['ssid']) +
                 encode('OptionalString', '52:54:00:87:87:01') +
                 encode('OptionalString', '') + struct.pack('!i', 1))

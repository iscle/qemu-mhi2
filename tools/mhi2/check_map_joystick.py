#!/usr/bin/env python3
"""Check native main-map scrolling, stop-on-release and hidden-map guarding."""
import os
import struct
from firmware_profile import PROFILES
from rcc_peer import Peer

os.environ['MHI2_MAP_JOYSTICK'] = '1'
peer = Peer.__new__(Peer)
peer.profile = dict(PROFILES['audi-a4'])
key = (b'\x0a\x00\x00\x0f', 21322, 40100)
flow = dict(client='mapviewer', map_instance=0, native_stub=123,
            map_ready=True, map_visible=True, connected=True)
peer.flows = {key: flow}
packets = []
peer.comm = lambda key, flow, data: packets.append(data)
for direction, angle in [('up', 0), ('right', 90), ('down', 180), ('left', 270)]:
    peer.input_event(dict(type='joystick', direction=direction))
    assert packets[-2] == b'\x06' + struct.pack('!HHi', 123, 62, 2)
    assert packets[-1] == b'\x06' + struct.pack('!HHi', 123, 99, angle)
    peer.input_event(dict(type='joystick', direction='neutral'))
    assert packets[-1] == b'\x06' + struct.pack('!HH', 123, 102)
# A direction sent while another page is open must not move a hidden map.
flow['map_visible'] = False
peer.input_event(dict(type='joystick', direction='right'))
assert struct.unpack('!HH', packets[-1][1:]) == (123, 102)
assert flow['joystick_direction'] == 'neutral'
# A press released during service connection must not start moving later.
flow.pop('native_stub')
peer.input_event(dict(type='joystick', direction='left'))
peer.input_event(dict(type='joystick', direction='neutral'))
flow.update(native_stub=123, map_visible=True)
peer.request_native_view(key, flow)
assert struct.unpack('!HH', packets[-1][1:]) == (123, 102)
print('Four native directions, neutral, hidden map and delayed release passed')
# Cursor changes only viewer state; the map receives a point-info request.
import json
import tempfile
from pathlib import Path
import mmi_touchpad
from mmi_touchpad import TouchDevice
from rcc_services import Services, SCHEMA
peer.touchpad = TouchDevice(True)
peer.touchpad.mode = 25
peer.services = Services.__new__(Services)
peer.services.definitions = SCHEMA['services']
flow.update(map_visible=True, map_ready=True, touch_position=None)
generation = peer.touchpad.generation
with tempfile.TemporaryDirectory(prefix='mhi2-cursor-check-') as folder:
    mmi_touchpad.CURSOR_PATH = Path(folder)/'cursor.json'
    packets.clear()
    def touch(action,x=500,y=500):
        peer.input_event(dict(type='touchpad',input_mode='map',action=action,
                              x=x,y=y,generation=generation))
    touch('press')
    assert packets[-1] == b'\x06'+struct.pack('!HH',123,102)
    touch('move',600,550)
    state = json.loads(mmi_touchpad.CURSOR_PATH.read_text())
    assert (state['x'],state['y'],state['visible']) == (614,264,True)
    # No dragMap, free-map mode, hot point or direction scroll while moving.
    assert all(struct.unpack('!H',p[3:5])[0]==102 for p in packets)
    touch('release',600,550)
    assert packets[-1] == b'\x06'+struct.pack('!HHBii',123,17,0,614,264)
    touch('press',600,550)
    peer.touchpad.mode = 12
    peer.touchpad.generation += 1
    touch('release',600,550)
    assert packets[-1] == b'\x06'+struct.pack('!HH',123,102)
    assert not json.loads(mmi_touchpad.CURSOR_PATH.read_text())['visible']
    assert flow['touch_position'] is None
print('Stationary-map cursor coordinates, point-info request and context-switch release passed')

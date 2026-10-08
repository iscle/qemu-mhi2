#!/usr/bin/env python3
"""Focused regression checks for the RCC wire protocol and simulated profile."""
import os
import tempfile
import contextlib
import io
import json
import struct
import unittest
import hashlib
from unittest.mock import patch
from pathlib import Path
from rcc_services import Services, encode, decode, SCHEMA_FILE
from rcc_persistence import Persistence, Reader, array
from rcc_peer import Peer, IP
from most_sink import TransportStream
from rcc_features import verify_data_signature, state_vector, DATA_MODULUS


class Transport:
    def __init__(self):self.frames=[]
    def sendall(self,data):self.frames.append(data[4:])


class Checks(unittest.TestCase):
    def test_identity_reaches_dsi_and_raw_persistence(self):
        from emulator_config import defaults, save
        config = defaults()
        config['identity']['vin'] = 'ZZZEMU00XP0000002'
        config['identity']['fazit_id'] = 'EMU-00009.10.2600000002'
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'config.json'
            save(config, path)
            with patch.dict(os.environ, {'MHI2_CONFIG': str(path)}):
                s = Services()
        names = ['DSICarVehicleStates']
        for name in names:
            spec = next(v for v in s.definitions[name]['replies'].values()
                        if v['name'] == 'updateVINData')
            values = s.initial(name, spec)
            _, wire = s.reply(name, 'updateVINData', values)
            reader = Reader(wire)
            self.assertEqual(decode('OptionalString', reader), config['identity']['vin'])
            self.assertEqual(reader.take(4), struct.pack('!i', 0x81))
        prefix = array([0]) + array([3221291024])
        replies = s.persistence.handle(7, prefix)
        self.assertEqual(len(replies), 1)
        mid, wire = replies[0]
        self.assertEqual(mid, 0)
        reader = Reader(wire)
        self.assertEqual(reader.array(), [0])
        self.assertEqual(reader.array(), [3221291024])
        self.assertEqual(reader.take(5), b'\0\0\0\0\1')
        self.assertEqual(bytes(reader.array('B')).decode(), config['identity']['fazit_id'])
        self.assertEqual(reader.array('i'), [0])  # Success, not missing attribute.
        self.assertEqual(reader.pos, len(wire))

    def test_display_power_firmware_key(self):
        for train, expected in (
                ('MHI2_ER_POG11_K5126', '0bfa6630-3507-5427-be78-4c68f807cc18'),
                ('MHI2_ER_VWG11_K3342', '7900f601-13e0-5850-a98b-287e195bd326'),
                ('MHI2_ER_AU37x_P5089', '7900f601-13e0-5850-a98b-287e195bd326')):
            with self.subTest(train=train), patch('rcc_services.FIRMWARE', {'release': train}), \
                    patch.dict(os.environ, {}, clear=True):
                self.assertEqual(Services().definitions['DisplayPower']['key'], expected)
                with patch.dict(os.environ, {'MHI2_DISPLAY_POWER_KEY': 'explicit-key'}):
                    self.assertEqual(Services().definitions['DisplayPower']['key'], 'explicit-key')

    def test_native_provider_ownership(self):
        registrations = {name for name, *_ in Services().registrations()}
        for name in ('DSIDataConnection', 'DSIDataConfiguration'):
            self.assertEqual(name in registrations,
                             SCHEMA_FILE != 'dsi_schema_pog11_k5126.json')

    def test_native_spy_contracts(self):
        s = Services()
        if SCHEMA_FILE != 'dsi_schema_pog11_k5126.json':
            self.assertFalse(any(n.startswith('Spy') for n in s.definitions))
            return
        # Native K5126 factory: MID 5 is a single-attribute subscription,
        # MID 14 is standstill, Bool occupies one byte, validFlag is Int32=1.
        self.assertEqual(s.handle('SpyGeneralVehicleStates', 5, bytes.fromhex('00000016')),
                         [(14, bytes.fromhex('ff00000001'))])
        self.assertEqual(s.handle('SpyGeneralVehicleStates', 5, bytes.fromhex('00000004')),
                         [(9, bytes.fromhex('0000000000000001'))])
        self.assertEqual(s.reply('SpyCarTimeUnitsLanguage', 'updateClockDate',
                                [{'year': 26, 'month': 10, 'day': 4}, 1]),
                         (6, bytes.fromhex('00001a0a0400000001')))
        for name, ids in [('SpyCarKombi', {38, 41}),
                          ('SpyCarVehicleStates', {21, 22, 23, 24})]:
            responses = s.handle(name, 3, b'')
            self.assertEqual({mid for mid, data in responses}, ids)
            self.assertTrue(all(data[-4:] == b'\0\0\0\1' for mid, data in responses))

    def test_sound_channels_and_range_requests(self):
        s = Services()
        def call(method, data):
            mid = next(int(k) for k,v in s.definitions['DSISound']['calls'].items()
                       if v.startswith(method+'('))
            return s.handle('DSISound', mid, data)
        channel = struct.pack('!ii', 0x57, 1)
        self.assertEqual(call('getMenuVolumeRange', channel),
                         [s.reply('DSISound', 'menuVolumeRange', [0x57, 1, 0, 30])])
        call('setVolume', channel + struct.pack('!h', 24))
        self.assertEqual(call('getVolume', channel),
                         [s.reply('DSISound', 'updateVolume', [0x57, 1, 24, 1])])
        self.assertEqual(call('getVolume', struct.pack('!ii', 0x52, 1)),
                         [s.reply('DSISound', 'updateVolume', [0x52, 1, 15, 1])])
        call('increaseVolume', channel + struct.pack('!h', 100))
        self.assertEqual(call('getVolume', channel),
                         [s.reply('DSISound', 'updateVolume', [0x57, 1, 30, 1])])
        with self.assertRaises(ValueError): call('getVolume', b'')

    def test_all_initial_notifications_encode(self):
        s = Services()
        for name, definition in s.definitions.items():
            for spec in definition['replies'].values():
                if spec['name'].startswith('update'):
                    with self.subTest(service=name, method=spec['name']):
                        s.reply(name, spec['name'], s.initial(name, spec))

    def test_tuner_has_bands_and_real_request_completion(self):
        s = Services()
        definition = s.definitions['DSIAMFMTuner']
        spec = next(v for v in definition['replies'].values() if v['name'] == 'updateWavebandInfoList')
        self.assertEqual([b['waveband'] for b in s.initial('DSIAMFMTuner', spec)[0]], [1, 3])
        mid = next(int(k) for k,v in definition['calls'].items() if v.startswith('selectStation('))
        result = s.handle('DSIAMFMTuner', mid, struct.pack('!iii', 88300, 0, 0))
        self.assertEqual(result[-1], s.reply('DSIAMFMTuner', 'selectStationStatus', [2]))
        self.assertEqual(s.values['DSIAMFMTuner', 'updateSelectedStation'][0]['frequency'], 88300)

    def test_native_startup_profile_record(self):
        p = Persistence()
        request = array([678364556])+array([21])
        # The native media API and HMI readInt require an integer callback,
        # with coding state in byte 1 and the selected profile in byte 0.
        self.assertEqual(p.handle(7, request),
                         [(1, request+array([0x101], 'i')+array([0], 'i'))])

    def test_garage_button_list_transaction(self):
        s = Services()
        # Actual startup request: all records, RA0, start 0, count 3, transaction 1.
        request = bytes.fromhex('00000000010000000000000000000000030000000100000001')
        expected = bytes.fromhex('00000000010000000000000000000000000000000100000001')
        self.assertEqual(s.handle('DSICarComfort', 212, request),
                         [(215, expected+encode('OptionalUGDOButtonListRA0VarArray', []))])

    def test_cluster_setup_does_not_start_video(self):
        peer = Peer(Transport(), io.BytesIO())
        peer.input_event({'type': 'cluster-setup'})
        key, flow = next(iter(peer.flows.items()))
        flow['native_stub'] = 123
        messages = []
        peer.comm = lambda key, flow, message: messages.append(message)
        peer.request_native_view(key, flow)
        self.assertEqual(messages, [b'\x06'+struct.pack('!HHii', 123, 55, 4, 2),
                                    b'\x06'+struct.pack('!HHiii', 123, 24, 70, 4, 0)])

    def test_cluster_video_waits_for_native_map_visibility(self):
        peer = Peer(Transport(), io.BytesIO())
        peer.input_event({'type':'cluster-map'})
        mk, mf = next(iter(peer.flows.items()))
        mf['native_stub'] = 123
        peer.input_event({'type':'cluster', 'context':70})
        dk, df = next((k, f) for k, f in peer.flows.items() if f['client']=='displaymanagement')
        df['native_stub'] = 124
        messages = []
        peer.comm = lambda key, flow, message: messages.append(message)
        start = b'\x06'+struct.pack('!HHii', 124, 57, 4, 10)
        peer.request_native_view(dk, df)
        self.assertNotIn(start, messages)
        for visible, validity in ((0, 0x81), (255, 0x82), (255, 0x81)):
            message = b'\x06'+struct.pack('!HHBi', 81, 142, visible, validity)
            mf['data'].extend(struct.pack('!I', len(message))+message)
            peer.messages(mk, mf)
            self.assertEqual(start in messages, visible == 255 and validity == 0x81)

    def test_pending_native_service_retries_without_tcp_reset(self):
        peer = Peer(Transport(), io.BytesIO())
        peer.input_event({'type':'cluster-map'})
        key, flow = next(iter(peer.flows.items()))
        flow['connected'] = True
        calls = []
        peer.comm = lambda key, flow, message: calls.append(message)
        with patch('rcc_peer.time.monotonic', return_value=100):
            peer.connect_native_view(key, flow)
        peer.next_connect = 1000
        with patch('rcc_peer.time.monotonic', return_value=106):
            peer.tick()
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0], calls[1])
        self.assertIs(peer.flows[key], flow)
        flow['native_stub'] = 123
        with patch('rcc_peer.time.monotonic', return_value=112):
            peer.tick()
        self.assertEqual(len(calls), 2)

    def test_async_error_uses_hmi_request_id(self):
        s = Services()
        # automaticProfile is wire method 3, but HMI tracks RT_AUTOMATICPROFILE
        # as 1004. A reply containing 3 leaves the request timer running.
        replies = s.handle('DSIDataConfiguration', 3, struct.pack('!i', 0))
        self.assertEqual(replies, [(2, encode('Int32', 1)
                        + encode('OptionalString', 'Not supported by simulated vehicle')
                        + encode('Int32', 1004))])
        for name, definition in s.definitions.items():
            for mid, request_id in definition.get('request_ids', {}).items():
                self.assertIn(mid, definition['calls'], name)
                self.assertGreaterEqual(request_id, 1000, name)

    def test_offline_data_acceptance(self):
        s = Services()
        # Captured HMI cancel request must complete rather than expire after 10s.
        self.assertEqual(s.handle('DSIDataConfiguration', 0, bytes.fromhex('0000006400')),
                         [(1, struct.pack('!i', 0))])
        self.assertEqual(s.handle('DSIDataConfiguration', 0,
                                  struct.pack('!i', 100)+encode('Bool', True)),
                         [(1, struct.pack('!i', 7))])
        with self.assertRaises(ValueError): s.handle('DSIDataConfiguration', 0, b'')

    def test_native_display_brightness(self):
        s = Services()
        self.assertEqual(s.handle('DisplayPower', 0, struct.pack('!I', 4)),
                         [(1, struct.pack('!Ibi', 4, 0, 0))])
        for step in (-5, 0, 5):
            s.handle('DisplayPower', 4, struct.pack('!Ib', 4, step))
            self.assertEqual(s.handle('DisplayPower', 0, struct.pack('!I', 4)),
                             [(1, struct.pack('!Ibi', 4, step, 0))])
        self.assertEqual(s.handle('DisplayPower', 0, struct.pack('!I', 0)),
                         [(1, struct.pack('!Ibi', 0, 0, 0))])
        with self.assertRaises(ValueError): s.handle('DisplayPower', 4, struct.pack('!Ib', 4, 6))
        self.assertEqual(s.handle('DisplayLvds', 0, struct.pack('!iI', 2, 123)),
                         [(1, struct.pack('!I', 123))])

    def test_companion_profile_isolation(self):
        s = Services()
        def call(name, method, args):
            mid = next(int(k) for k,v in s.definitions[name]['calls'].items()
                       if v.startswith(method+'('))
            return s.handle(name, mid, struct.pack('!'+'i'*len(args), *args))
        for name in ('DSISound', 'DSIAMFMTuner', 'DSIDABTuner'):
            spec = next(v for v in s.definitions[name]['replies'].values()
                        if v['name'] == 'updateProfileState')
            self.assertEqual(s.initial(name, spec), [2, 0, 129])
            s.values[name, 'updateTestValue'] = [[15]]
            call(name, 'profileCopy', [0, 1])
            call(name, 'profileChange', [1])
            s.values[name, 'updateTestValue'][0][0] = 22
            call(name, 'profileChange', [0])
            self.assertEqual(s.values[name, 'updateTestValue'], [[15]])
            call(name, 'profileChange', [1])
            self.assertEqual(s.values[name, 'updateTestValue'], [[22]])
            call(name, 'profileReset', [1])
            self.assertNotIn((name, 'updateTestValue'), s.values)
            self.assertEqual(s.initial(name, spec), [2, 1, 129])
        self.assertEqual(call('DSIKeyPanel', 'setGenericSetting', [13, 109, 255]),
                         [(4, struct.pack('!iii', 13, 109, 255))])
        self.assertEqual(call('DSIKeyPanel', 'requestGenericSetting', [13, 109]),
                         [(4, struct.pack('!iii', 13, 109, 255))])

    def test_production_map_signature(self):
        digest = bytes.fromhex('4c8b0c4b6de9e2b006be3c319b52137fd4867040')
        signature = bytes.fromhex(
            '33b88fb195694df378f72b2ee532e6ab45dd47359e6af98365fdff127e04e02c'
            '4396892de658fbe77bb6b8fb95e552c8850cf02b0209bc6e4ce832064f5f2fe6'
            'd8e18e917a64e34400491d43cdb70a58bd82a395c65e58d57976eff33ff5c264'
            'b0b9c6c3a714868b1c8af7b4a8db5b1d84eea78a1f61ee88c17599ccdfac7279')
        self.assertTrue(verify_data_signature(digest, signature))
        for d, sig in ((digest[:-1], signature), (digest, signature[:-1]),
                       (b'\0'*20, signature), (digest, b'\0'*128),
                       (digest, DATA_MODULUS.to_bytes(128, 'big'))):
            self.assertFalse(verify_data_signature(d, sig))
        s = Services()
        manifest = b'example manifest bytes sent by the native navigation client'
        wire = encode('OptionalString', 'content.pkg')+array(manifest, 'B')+array(signature, 'B')
        with patch('rcc_services.verify_data_signature', return_value=True) as verify:
            self.assertEqual(s.handle('FecManager', 0, wire),
                             [(1, encode('OptionalString', 'content.pkg')+b'\xff')])
            verify.assert_called_once_with(hashlib.sha1(manifest).digest(), signature)
        with self.assertRaises(ValueError):
            s.handle('FecManager', 0, wire+b'\0')

    def test_audio_connection_lifecycle(self):
        s = Services()
        def call(method, args):
            mid = next(int(k) for k,v in s.definitions['DSIAudioManagement']['calls'].items()
                       if v.startswith(method+'('))
            return s.handle('DSIAudioManagement', mid, struct.pack('!'+'i'*len(args), *args))
        self.assertEqual(call('requestConnection', [12, 1, 0]), [(18, struct.pack('!ii', 12, 1))])
        self.assertEqual(call('fadeToConnection', [12, 1])[-1], (6, struct.pack('!ii', 12, 1)))
        self.assertEqual(call('getActiveConnection', [1]), [(21, struct.pack('!iii', 12, 1, 1))])
        call('releaseConnection', [12, 1])
        self.assertEqual(call('getActiveConnection', [1]), [(21, struct.pack('!iii', 0, 1, 1))])

    def test_native_video_connection(self):
        s = Services()
        request = struct.pack('!ii', 0, 3)
        self.assertEqual(s.handle('VideoConnection', 9, request),
                         [(10, request + struct.pack('!i', 0))])
        self.assertEqual(s.handle('VideoConnection', 8, request),
                         [(10, request + struct.pack('!i', 1))])
        unsupported = struct.pack('!ii', 1, 3)
        self.assertEqual(s.handle('VideoConnection', 9, unsupported),
                         [(10, unsupported + struct.pack('!i', 2))])
        self.assertEqual(s.handle('VideoConnection', 4, bytes(4)),
                         [(7, struct.pack('!ii', 0, 10))])
        for mid, payload in ((8, b''), (9, request + b'\0'), (4, request)):
            with self.assertRaises(ValueError):
                s.handle('VideoConnection', mid, payload)

    def test_native_map_control_coordinates(self):
        peer = Peer(Transport(), io.BytesIO())
        peer.input_event({'type':'cluster-map', 'latitude':41.4, 'longitude':2.17})
        key, flow = next(iter(peer.flows.items()))
        replies = []
        peer.comm = lambda key, flow, message: replies.append(message)
        ack = b'\x0d' + struct.pack('!HHH', 80, 123, 81)
        flow['data'].extend(struct.pack('!I', len(ack)) + ack)
        peer.messages(key, flow)
        self.assertNotIn(b'\x06' + struct.pack('!HHB', 123, 148, 255), replies)
        ready = b'\x06'+struct.pack('!HHBi', 81, 126, 255, 0x81)
        flow['data'].extend(struct.pack('!I', len(ready)) + ready)
        peer.messages(key, flow)
        self.assertIn(b'\x06' + struct.pack('!HHBii', 123, 56, 0,
                          int(2.17 * 11930464), int(41.4 * 11930464)), replies)
        self.assertIn(b'\x06' + struct.pack('!HHB', 123, 148, 255), replies)
        peer.input_event({'type':'cluster-map'})
        self.assertEqual(len(peer.flows), 1)
        del flow['native_stub']
        flow['created'] -= 20
        flow['connected'] = True
        peer.input_event({'type':'cluster-map'})
        self.assertIn(key, peer.flows, 'Accepted native connection reset during initialization')
        flow['connected'] = False
        peer.input_event({'type':'cluster-map'})
        self.assertEqual(len(peer.flows), 1)
        self.assertNotIn(key, peer.flows)
        for event in ({'instance':2}, {'latitude':91,'longitude':0},
                      {'latitude':0,'longitude':float('nan')}):
            with self.assertRaises(ValueError):
                peer.input_event({'type':'cluster-map', **event})

    def test_persistence_identification(self):
        p=Persistence()
        replies=p.handle(7,array([46924065])+array([400]))
        self.assertEqual(replies[0][0],0)
        r=Reader(replies[0][1]);self.assertEqual(r.array(),[46924065]);self.assertEqual(r.array(),[400])
        self.assertEqual(r.take(5),b'\0\0\0\0\1')
        blob=bytes(r.array('B'));self.assertEqual(len(blob),29)
        self.assertEqual(blob[22:26],b'1427')
        self.assertEqual(r.array('i'),[0])
        self.assertEqual(r.pos,len(r.data))

    def test_persistence_write_read_and_missing(self):
        p=Persistence();prefix=array([123])+array([456])
        result=p.handle(3,prefix+array([42],'i'))
        self.assertEqual(result[0],(4,prefix+array([0],'i')))
        self.assertEqual(p.handle(7,prefix),[(1,prefix+array([42],'i')+array([0],'i'))])
        missing=p.handle(7,array([123])+array([999]))
        self.assertEqual(missing[0][1][-4:],struct.pack('!i',1))
        with self.assertRaises(ValueError):p.handle(3,prefix+array([],'i'))

    def test_eso_encoding(self):
        self.assertEqual(encode('Bool', True), b'\xff')
        self.assertEqual(encode('Bool', False), b'\0')
        self.assertEqual(encode('OptionalString','MU1427'),b'\0\0\0\6MU1427')
        self.assertEqual(encode('OptionalCarViewOption',{'state':2,'reason':0}),b'\0'+struct.pack('!ii',2,0))
        self.assertEqual(encode('OptionalInt32VarArray',[1,2]),b'\0'+struct.pack('!Iii',2,1,2))

    def test_subscriptions_and_inventory(self):
        with tempfile.TemporaryDirectory() as root:
            metadata=Path(root)/'metainfo2.txt'
            metadata.write_text('[MMX2\\qb-primary\\70\\default\\File]\nLink = "[MMX2\\qb-primary\\50\\default\\File]"\n[MMX2\\qb-primary\\50\\default\\File]\nVersion = "155"\n')
            with patch.dict(os.environ, MHI2_FIRMWARE_META=str(metadata)):
                s=Services()
        # Wire event changed between VW K3342 and Porsche K5126.
        reply=s.handle('DSICarKombi',34,array([4]))
        self.assertEqual(reply[0][0],207 if SCHEMA_FILE == 'dsi_schema_pog11_k5126.json' else 186)
        self.assertEqual(reply[0][1][-4:],struct.pack('!i',129))
        mmx=next(d for d in s.inventory if d['name']=='MMX2')
        self.assertTrue(all(m['hw']==70 for m in mmx['modules']))
        quickboot=next(m for m in mmx['modules'] if m['name'].startswith('qb-primary/'))
        self.assertEqual(quickboot['version'],155)
        date={'name':'updateClockDate','types':['OptionalClockDate','Int32']}
        # The firmware adds 2000 to this wire year.
        self.assertLess(s.initial('DSICarTimeUnitsLanguage',date)[0]['year'],100)
        for name,d in s.definitions.items():
            if name=='Attributes':continue
            for spec in d['replies'].values():
                if spec['name'].startswith('update'):
                    s.reply(name,spec['name'],s.initial(name,spec))
        with self.assertRaises(ValueError):s.handle('DSICarKombi',34,b'\0'+struct.pack('!I',1000000))

    def test_most_native_block_header_and_sync(self):
        sink=TransportStream()
        def packet(cc):return bytes([0x47,0x01,0,0x10|cc])+bytes(184)
        ts=packet(0)+packet(1)+packet(3)
        self.assertEqual(sink.extract(bytes(24)+ts),ts)
        self.assertEqual(sink.packets,3)
        self.assertEqual(sink.discontinuities,1)
        self.assertEqual(sink.extract(b'not MPEG TS'),b'')

    def test_large_tcp_message_is_segmented_and_retransmitted(self):
        transport=Transport();peer=Peer(transport,io.BytesIO())
        key=(bytes([10,0,0,15]),21100,40000)
        flow={'seq':100,'ack':99,'mac':peer.guest_mac,'data':bytearray()}
        peer.flows[key]=flow
        peer.tcp(key,flow,0x18,b'x'*16000)
        self.assertEqual(len(transport.frames),8)
        self.assertTrue(all(len(f)<=1454 for f in transport.frames))
        flow['pending'][0]['sent']-=2
        peer.flush_tcp(key,flow)
        self.assertEqual(transport.frames[0],transport.frames[-1])
        acknowledged=flow['pending'][7]['end']
        # Guest ACK advances the transmit window; remaining segments are sent.
        tcp=struct.pack('!HHIIBBHHH',21100,40000,99,acknowledged,0x50,0x10,32768,0,0)
        ip=struct.pack('!BBHHHBBH4s4s',0x45,0,40,0,0,64,6,0,key[0],IP)
        peer.receive(bytes(6)+peer.guest_mac+b'\x08\x00'+ip+tcp)
        self.assertEqual(len(flow['pending']),4)
        self.assertEqual(len(transport.frames),13)


if __name__=='__main__':
    with contextlib.redirect_stdout(io.StringIO()):
        unittest.main()

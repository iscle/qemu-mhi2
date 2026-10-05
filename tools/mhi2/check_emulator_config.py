#!/usr/bin/env python3
"""Offline checks: firmware selection, feature wire states, USB setup ownership."""
import json
import os
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from emulator_config import defaults, load, save, validate
from firmware_profile import common_metadata, select_profile
from rcc_features import FEATURES, state_vector
from usb_transport import USBTransport
from rcc_persistence import Persistence


class Checks(unittest.TestCase):
    def test_porsche_equipment_matches_feature_permissions(self):
        config = defaults()
        meta = {'release': 'MHI2_ER_POG11_K5126'}
        enabled = Persistence(config, meta).values
        coding = bytes.fromhex(enabled['28180695:1']['value'])
        adap = bytes.fromhex(enabled['28442848:100']['value'])
        self.assertEqual(coding[19] >> 6, 3)
        self.assertEqual(coding[0] & 0xf, 7)
        train = bytes.fromhex(enabled['46924065:401']['value'])
        self.assertGreaterEqual(len(train), 21)
        self.assertEqual(train.rstrip().decode(), meta['release'])
        self.assertEqual([adap[i] & 1 for i in range(30, 34)], [1, 0, 0, 0])
        self.assertTrue(adap[43] & 0x80)
        self.assertTrue(adap[51] & 1)
        for code in ('00060900', '00060800', '00060300', '00060b00'):
            config['features'][code] = False
        disabled = bytes.fromhex(Persistence(config, meta).values['28442848:100']['value'])
        for i, mask in ((43, 0xc0), (51, 0x61), (70, 4)):
            self.assertEqual(disabled[i] & mask, 0)
            self.assertEqual(disabled[i] & ~mask, adap[i] & ~mask)
        original = json.loads(Path(__file__).with_name('vehicle_profile.json').read_text())
        self.assertEqual(Persistence(config, {}).values, original)

    def test_firmware_selection_ignores_audi_header(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'metainfo2.txt'
            path.write_text('# Audi components\n[common]\nrelease = "MHI2_ER_POG11_K5126"\n'
                            '[component]\nrelease = "MHI2_ER_VWG11_K3342"\n')
            self.assertEqual(select_profile(common_metadata(path))['brand'], 'porsche')
        self.assertEqual(select_profile({'release': 'MHI2_ER_VWG11_K3342'})['brand'], 'volkswagen')
        self.assertEqual(select_profile({})['brand'], 'volkswagen')
        self.assertEqual(select_profile({'release': 'MHI2_ER_AUG22_P3663'})['brand'], 'generic')

    def test_feature_persistence_and_wire_states(self):
        config = defaults()
        config['features']['00060900'] = False
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'settings.json'
            save(config, path)
            config = load(path)
            wire = state_vector(config)
            self.assertEqual(struct.unpack_from('!BI', wire), (0, len(FEATURES)))
            entries = list(struct.iter_unpack('!BIIi', wire[5:]))
            self.assertEqual([v[2] for v in entries], list(range(len(FEATURES))))
            states = {v[1]: v[3] for v in entries}
            self.assertEqual(states[0x60900], 0)
            self.assertEqual(states[0x60800], 1)
            before = path.read_bytes()
            config['features']['unknown'] = True
            with self.assertRaises(ValueError): save(config, path)
            self.assertEqual(path.read_bytes(), before)

    def test_reject_invalid_and_overbroad_usb_settings(self):
        for update in ({'mode': 'host'}, {'hostport': '2,id=other'},
                       {'port': 22}, {'port': True}, {'serial': 'emulator 5554'}):
            config = defaults()
            config['usb'].update(update)
            with self.assertRaises(ValueError): validate(config)

    def mock_adb(self, transport, stub=False, existing=''):
        calls = []
        def run(*args, **kwargs):
            calls.append(args)
            if args == ('devices',): return 'List of devices attached\nemulator-5554\tdevice\n'
            if args[:2] == ('shell', 'dumpsys'):
                return 'versionName=1.2-stub' if stub else 'versionName=16.0'
            if args == ('forward', '--list'): return existing
            return ''
        transport.adb_run = run
        return calls

    def test_avd_forward_lifecycle(self):
        config = defaults()['usb']; config['mode'] = 'avd'
        transport = USBTransport(config)
        calls = self.mock_adb(transport)
        args = transport.prepare()
        self.assertIn('usb-android-auto,id=phone,bus=usb-bus.0,port=1,chardev=androidauto', args)
        self.assertIn(('forward', '--no-rebind', 'tcp:5277', 'tcp:5277'), calls)
        transport.close()
        self.assertIn(('forward', '--remove', 'tcp:5277'), calls)

    def test_porsche_media_connector_topology(self):
        config = defaults()['usb']; config['mode'] = 'avd'
        transport = USBTransport(config)
        self.mock_adb(transport)
        with patch('usb_transport.common_metadata', return_value={'release': 'MHI2_ER_POG11_K5126'}):
            args = transport.prepare()
        self.assertIn('usb-mhi2-hub,id=mediahub,bus=usb-bus.0,port=1,ports=3,port-power=on', args)
        self.assertIn('usb-mhi2-hfc,id=mediahfc,bus=usb-bus.0,port=1.3', args)
        self.assertIn('usb-android-auto,id=phone,bus=usb-bus.0,port=1.1,chardev=androidauto', args)
        self.assertTrue(args[0].startswith('-'))

    def test_avd_stub_rejected_without_forwarding(self):
        config = defaults()['usb']; config['mode'] = 'avd'
        transport = USBTransport(config)
        calls = self.mock_adb(transport, stub=True)
        with self.assertRaisesRegex(ValueError, 'stub'): transport.prepare()
        self.assertFalse(any(c[0] == 'forward' for c in calls))

    def test_existing_forward_is_not_removed(self):
        config = defaults()['usb']; config['mode'] = 'avd'
        transport = USBTransport(config)
        calls = self.mock_adb(transport, existing='emulator-5554 tcp:5277 tcp:5277\n')
        transport.prepare(); transport.close()
        self.assertNotIn(('forward', '--remove', 'tcp:5277'), calls)
        transport = USBTransport(config)
        self.mock_adb(transport, existing='emulator-5556 tcp:5277 tcp:1234\n')
        with self.assertRaises(ValueError): transport.prepare()


if __name__ == '__main__':
    unittest.main()

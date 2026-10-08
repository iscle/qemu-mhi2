#!/usr/bin/env python3
"""Check recovered native network requests and isolation between clients."""
import contextlib
import io
import json
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

import native_network
import rcc_services
from rcc_persistence import Reader


class Peer:
    def __init__(self):
        self.local_port = 41000
        self.guest_mac = bytes.fromhex('020002878701')
        self.flows = {}
        self.calls = []

    def tcp(self, *args):
        pass

    def comm(self, key, flow, payload):
        self.calls.append(payload)


class Checks(unittest.TestCase):
    def setUp(self):
        self.peer = Peer()
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        schema = json.loads(Path(__file__).with_name(
            'dsi_schema_pog11_k5126.json').read_text())
        stack.enter_context(patch.object(rcc_services, 'SCHEMA', schema))
        stack.enter_context(patch('firmware_profile.common_metadata',
                               return_value={'release': 'MHI2_ER_POG11_K5126'}))
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))

    def start(self, action):
        native_network.start(self.peer, dict(action=action))
        key = next(reversed(self.peer.flows))
        flow = self.peer.flows[key]
        native_network.message(self.peer, key, flow,
                               b'\x0d' + struct.pack('!HHH', 80, 123, 81))
        return key, flow

    def test_distinct_clients_and_single_context_profile(self):
        _, wifi = self.start('wifi')
        _, cellular = self.start('cellular')
        self.assertNotEqual(wifi['local_agent'], cellular['local_agent'])
        prefix = b'\x06' + struct.pack('!HH', 123, 36)
        profiles = [p[5:] for p in self.peer.calls if p[:5] == prefix]
        self.assertEqual(len(profiles), 1)
        profile = rcc_services.decode('OptionalCDataProfile', Reader(profiles[0]))
        self.assertEqual(profile['dataAPN'], 'qemu')
        self.assertEqual(profile['dataAPN2'], '')
        self.assertEqual(profile['dataPassword'], '')
        self.start('cellular')
        self.assertEqual(len(self.peer.flows), 2)

    def test_search_reply_is_count_then_result(self):
        key, flow = self.start('wifi')
        self.peer.calls.clear()
        prefix = b'\x06' + struct.pack('!HH', 81, 51)
        native_network.message(self.peer, key, flow, prefix + struct.pack('!ii', 1, 6))
        self.assertEqual(self.peer.calls, [])
        native_network.message(self.peer, key, flow, prefix + struct.pack('!ii', 1, 0))
        self.assertEqual(self.peer.calls[0][:5], b'\x06' + struct.pack('!HH', 123, 59))

    def test_rejects_other_firmware_and_malformed_replies(self):
        with patch('firmware_profile.common_metadata', return_value={}):
            with self.assertRaises(ValueError):
                native_network.start(self.peer, dict(action='cellular'))
        key, flow = self.start('cellular')
        for payload in (b'\x01', b'\x06', b'\x0d'):
            native_network.message(self.peer, key, flow, payload)


if __name__ == '__main__':
    unittest.main()

import os
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from PySide6.QtWidgets import QApplication, QLabel
from rcc_peer import Peer
from firmware_profile import select_profile
from view_ui import Panel

APP = QApplication.instance() or QApplication([])
PROFILE = select_profile({'release': 'MHI2_ER_SKG13_P4526', 'MUVersion': '1440'})


def write_ppm(path, width, height, color):
    header = f'P6\n{width} {height}\n255\n'.encode('ascii')
    path.write_bytes(header + bytes(color) * (width * height))


class ViewerAndInputTests(unittest.TestCase):
    def test_viewer_maps_guest_dimensions_across_resize_and_rejects_bad_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            frame_path = Path(temporary) / 'frame.ppm'
            os.environ['MHI2_GL_FRAME'] = str(frame_path)
            status = QLabel()
            panel = Panel(status, PROFILE)
            try:
                for size, color in (((1280, 640), (220, 20, 30)),
                                    ((1000, 700), (20, 210, 40)),
                                    ((900, 900), (30, 40, 220))):
                    panel.resize(*size)
                    write_ppm(frame_path, 1280, 640, color)
                    panel.refresh()
                    self.assertFalse(panel.frame.isNull())
                    self.assertEqual(panel.frame.pixelColor(10, 10).getRgb()[:3], color)
                    rect = panel.frame_rect()
                    center = panel.guest_point(rect.center())
                    self.assertLessEqual(abs(center.x() - 640), 2)
                    self.assertLessEqual(abs(center.y() - 320), 2)
                    self.assertEqual(panel.guest_point(rect.topLeft()).x(), 0)
                    self.assertEqual(panel.guest_point(rect.topLeft()).y(), 0)

                prior = panel.frame.copy()
                write_ppm(frame_path, 800, 480, (1, 2, 3))
                panel.refresh()
                self.assertEqual(panel.frame.size(), prior.size())
                self.assertEqual(panel.frame.pixelColor(0, 0), prior.pixelColor(0, 0))

                frame_path.write_bytes(b'P6\n1280 640\n255\nshort')
                time.sleep(0.002)
                os.utime(frame_path, None)
                panel.refresh()
                self.assertEqual(panel.frame.pixelColor(0, 0), prior.pixelColor(0, 0))

                write_ppm(frame_path, 1280, 640, (7, 8, 9))
                with frame_path.open('ab') as stream:
                    stream.write(b'!')
                time.sleep(0.002)
                os.utime(frame_path, None)
                panel.refresh()
                self.assertEqual(panel.frame.pixelColor(0, 0), prior.pixelColor(0, 0))
            finally:
                panel.close()
                os.environ.pop('MHI2_GL_FRAME', None)

    def test_legacy_profiles_keep_800x480_frames_and_guest_coordinates(self):
        profiles = (
            ('audi', {'release': 'MHI2_ER_AU37x_P5089'}, (201, 31, 41)),
            ('volkswagen', {'release': 'MHI2_ER_VWG11_K3342'}, (31, 201, 41)),
            ('porsche', {'release': 'MHI2_ER_POG11_K5126'}, (31, 41, 201)),
        )
        with tempfile.TemporaryDirectory() as temporary:
            frame_path = Path(temporary) / 'legacy.ppm'
            os.environ['MHI2_GL_FRAME'] = str(frame_path)
            try:
                for expected_brand, metadata, color in profiles:
                    with self.subTest(brand=expected_brand):
                        profile = select_profile(metadata)
                        self.assertEqual(profile['brand'], expected_brand)
                        status = QLabel()
                        panel = Panel(status, profile)
                        try:
                            panel.resize(1000, 700)
                            write_ppm(frame_path, 800, 480, color)
                            panel.refresh()
                            self.assertEqual((panel.native_size.width(),
                                              panel.native_size.height()), (800, 480))
                            self.assertEqual(panel.frame.size().width(), 800)
                            self.assertEqual(panel.frame.size().height(), 480)
                            self.assertEqual(panel.frame.pixelColor(10, 10).getRgb()[:3], color)
                            rect = panel.frame_rect()
                            center = panel.guest_point(rect.center())
                            self.assertLessEqual(abs(center.x() - 400), 2)
                            self.assertLessEqual(abs(center.y() - 240), 2)
                            self.assertEqual(panel.guest_point(rect.topLeft()).x(), 0)
                            self.assertEqual(panel.guest_point(rect.topLeft()).y(), 0)
                        finally:
                            panel.close()
            finally:
                os.environ.pop('MHI2_GL_FRAME', None)
    def test_touch_wire_divisor_applies_once_and_bounds_are_guest_native(self):
        peer = Peer.__new__(Peer)
        peer.profile = PROFILE
        key = ('fixture', 0, 0)
        flow = {'keypanel_reply': 42}
        peer.flows = {key: flow}
        sent = []
        peer.comm = lambda k, f, packet: sent.append(packet)
        peer.input_event({'type': 'touch', 'action': 'tap', 'x': 440, 'y': 320})
        self.assertEqual(len(sent), 1)
        self.assertEqual(struct.unpack('!HH', sent[0][1:5]), (42, 42))
        fields = struct.unpack('!iiiBiiiiii', sent[0][5:])
        self.assertEqual(fields[0:4], (13, 1, 1, 0))
        self.assertEqual(fields[4:6], (220, 160))
        with self.assertRaisesRegex(ValueError, 'outside 1280x640'):
            peer.input_event({'type': 'touch', 'action': 'tap', 'x': 1280, 'y': 0})


if __name__ == '__main__':
    unittest.main()

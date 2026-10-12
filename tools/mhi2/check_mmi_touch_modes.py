#!/usr/bin/env python3
"""Check immediate map gestures and the separate handwriting mode."""
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import json
import tempfile
import time
from pathlib import Path
from PySide6.QtCore import Qt, QPoint
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication
import mmi_touch_widget as ui
from mmi_touchpad import TouchDevice
from rcc_services import Services, SCHEMA

services = Services.__new__(Services)
services.definitions = SCHEMA['services']
device = TouchDevice(True)
device.mode = 25
for action in ('press', 'move', 'release'):
    mid, payload = device.event(services, dict(action=action, input_mode='map',
        x=500, y=500, generation=device.generation))
    assert payload[:12] == (9).to_bytes(4,'big') + bytes(4) + int(action != 'release').to_bytes(4,'big')
for event in [dict(action='character', character='A', input_mode='map'),
              dict(action='press', x=500, y=500, input_mode='characters')]:
    try:
        device.event(services, dict(event, generation=device.generation))
    except ValueError:
        pass
    else:
        raise AssertionError('Map/character mode isolation failed')
device.mode = 12
assert device.event(services, dict(action='character', character='A', generation=device.generation))[1]

app = QApplication([])
class Panel:
    def __init__(self): self.events = []
    def send(self, event): self.events.append(event)
with tempfile.TemporaryDirectory(prefix='mhi2-touch-mode-') as folder:
    ui.STATE_PATH = Path(folder)/'state.json'
    def state(mode):
        ui.STATE_PATH.write_text(json.dumps(dict(enabled=True, ready=True, mode=mode,
                                                generation=1, updated=time.time())))
    state(25)
    panel = Panel()
    widget = ui.TouchpadWidget(panel)
    widget.show();app.processEvents()
    assert widget.map_mode and widget.active
    QTest.mousePress(widget.surface, Qt.LeftButton, pos=QPoint(30,80))
    QTest.mouseMove(widget.surface, QPoint(100,80))
    assert [e['action'] for e in panel.events] == ['press','move']
    assert all(e['input_mode']=='map' for e in panel.events)
    QTest.mouseRelease(widget.surface, Qt.LeftButton, pos=QPoint(150,80))
    assert panel.events[-2]['action']=='move' and panel.events[-1]['action']=='release'
    assert not widget.finish_timer.isActive() and not widget.candidates.count()
    # Switching during contact must release the old interaction.
    QTest.mousePress(widget.surface, Qt.LeftButton, pos=QPoint(30,80))
    state(12);widget.input_mode.setCurrentIndex(1)
    assert panel.events[-1]['action']=='release' and panel.events[-1]['input_mode']=='map'
    assert not widget.map_mode and widget.active
    widget.close()
print('Native gesture serialization, immediate moves, final point, mode switch release and handwriting passed')

# The overlay remains visible even when the guest framebuffer is unchanged.
from PySide6.QtGui import QImage, QColor
from view_ui import Panel as FramePanel
from firmware_profile import PROFILES
from PySide6.QtWidgets import QLabel
frame = FramePanel(QLabel(), PROFILES['audi-a4'])
frame.timer.stop();frame.resize(1024,480)
frame.frame = QImage(1024,480,QImage.Format_RGB32)
frame.frame.fill(QColor('#336644'))
frame.map_cursor = (512,240)
first = frame.grab().toImage()
assert first.pixelColor(525,240).red()>200
frame.map_cursor = (614,264)
second = frame.grab().toImage()
assert second.pixelColor(627,264).red()>200
assert second.pixelColor(525,240)==QColor('#336644')
print('Cursor overlay moves over an unchanged framebuffer')

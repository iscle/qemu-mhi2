#!/usr/bin/env python3
"""Check firmware input routing and the desktop controller without a VM."""
import os
import struct
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QImage
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QPushButton
from firmware_profile import current
from rcc_peer import Peer
from view_ui import Panel, create_window

app = QApplication([])
for name, keyboard in [('vw', 13), ('audi-a3', 1), ('porsche', 13)]:
    os.environ['MHI2_FIRMWARE'] = name
    peer = Peer.__new__(Peer)
    peer.profile = current()
    peer.flows = {0: {'keypanel_reply': 123}}
    packets = []
    peer.comm = lambda key, flow, data: packets.append(data)
    peer.input_event(dict(type='key', code=78, pressed=1))
    peer.input_event(dict(type='encoder', code=16, ticks=-2))
    assert struct.unpack('!HH', packets[0][1:5]) == (123, 38)
    assert struct.unpack('!iii', packets[0][5:17]) == (keyboard, 78, 1)
    assert struct.unpack('!HHiii', packets[1][1:17]) == (123, 30, keyboard, 16, -2)
    if name == 'audi-a3':
        try:
            peer.input_event(dict(type='touch', action='tap', x=100, y=100))
        except ValueError:
            pass
        else:
            raise AssertionError('Audi accepted a touchscreen event')
    window = create_window()
    window.show()
    app.processEvents()
    panel = window.findChild(Panel)
    events = []
    panel.send = events.append
    window.resize(900, 600)
    app.processEvents()
    assert window.height() <= 600, (name, window.minimumSizeHint())
    assert window.maximumHeight() > 600
    frame = panel.frame_rect()
    QTest.mouseClick(panel, Qt.LeftButton, pos=frame.center())
    if name != 'audi-a3':
        assert [event['action'] for event in events] == ['press', 'tap', 'release']
        assert abs(events[0]['x'] - 400) <= 3
        assert abs(events[0]['y'] - 240) <= 3
        events.clear()
        # Letterbox clicks must not operate controls at the screen edge.
        if not frame.contains(QPoint(0, 0)):
            QTest.mouseClick(panel, Qt.LeftButton, pos=QPoint(1, 1))
            assert events == []
        QTest.mousePress(panel, Qt.LeftButton, pos=frame.center())
        QTest.mouseRelease(panel, Qt.LeftButton, pos=QPoint(panel.width()+10, panel.height()+10))
        assert events[-1] == dict(type='touch', action='release', x=799, y=479)
    else:
        assert events == []
        QTest.mouseClick(window.findChild(QPushButton, 'menu'), Qt.LeftButton)
        assert [event['pressed'] for event in events] == [1, 0]
        assert all(event['code'] == 78 for event in events)
        events.clear()
        QTest.mouseClick(window.findChild(QPushButton, 'encoder_16_1'), Qt.LeftButton)
        assert events == [dict(type='encoder', code=16, ticks=1)]
        events.clear()
        QTest.keyClick(window, Qt.Key_Left)
        assert events == [dict(type='encoder', code=16, ticks=-1)]
    if name == 'porsche':
        assert window.windowTitle() == 'Porsche PCM 4.0'
        assert window.findChild(QPushButton, 'home') is not None
        assert window.findChild(QPushButton, 'cluster_display').text() == 'Cluster display'
        events.clear()
        QTest.mouseClick(window.findChild(QPushButton, 'home'), Qt.LeftButton)
        assert [event['code'] for event in events] == [78, 78]
    # A paused cluster frame must scale immediately, without a new file frame.
    cluster = window.cluster
    cluster.timer.stop()
    cluster.show()
    cluster.screen.frame = QImage(408 if name == 'porsche' else 800,
                                  448 if name == 'porsche' else 400, QImage.Format_RGB32)
    cluster.screen.frame.fill(Qt.red)
    cluster.resize(360, 400)
    app.processEvents()
    assert cluster.height() <= 400
    before = cluster.screen.frame_rect().size()
    cluster.resize(500, 550)
    app.processEvents()
    assert cluster.screen.frame_rect().size() != before
    assert cluster.screen.grab().toImage().pixelColor(cluster.screen.rect().center()) == Qt.red
    cluster.close()
    window.close()
    print(name, 'input routing and controls passed')

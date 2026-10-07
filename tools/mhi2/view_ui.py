#!/usr/bin/env python3
"""Display experimental GLES frames and send production RCC keypanel events."""
import json
import argparse
import os
import sys
import time
from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QImage, QPainter, QPixmap, QShortcut, QKeySequence
from PySide6.QtWidgets import QApplication, QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QLabel, QMessageBox
from firmware_profile import common_metadata, select_profile

from firmware_profile import current

# Firmware org.dsi.ifc.keypanel.Constants; profile selects ABT/FCC identity.
LEFT_KEYS = [('RADIO', 15), ('MEDIA', 1), ('PHONE', 3), ('VOICE', 50)]
RIGHT_KEYS = [('NAV', 4), ('TRAFFIC', 5), ('CAR', 6), ('MENU', 78)]
# Porsche PCM 4 controls. K5126's active key mapping opens Home on MENU=78;
# the generic keypanel HOME=116 is ignored. Layout: MY18 PCM quick reference.
PORSCHE_KEYS = [(('TUNER', 15), ('SOURCE', 103)),
                (('MEDIA', 1), ('PHONE', 3)),
                (('NAV', 4), ('CAR', 6)),
                (('MAP', 104), ('HOME', 78))]


class Panel(QWidget):
    def __init__(self, status, profile=None):
        super().__init__()
        self.setFixedSize(800, 480)
        self.status = status
        self.touch_enabled = (current() if profile is None else profile)["touch"]
        self.frame = QImage()
        self.stamp = None
        self.pressed = None
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(50)

    def send(self, event):
        try:
            fd = os.open('/tmp/mhi2-rcc-input', os.O_WRONLY | os.O_NONBLOCK)
            try:
                os.write(fd, (json.dumps(event) + '\n').encode())
            finally:
                os.close(fd)
            self.status.setText('Input sent')
        except OSError as exc:
            self.status.setText(f'RCC input unavailable: {exc.strerror}')

    def touch(self, action, point):
        self.send(dict(type='touch', action=action, x=max(0, min(799, point.x())),
                       y=max(0, min(479, point.y()))))

    def mousePressEvent(self, event):
        if self.touch_enabled and event.button() == Qt.LeftButton:
            self.pressed = event.position().toPoint()
            self.touch('press', self.pressed)

    def mouseMoveEvent(self, event):
        if self.pressed is not None:
            self.touch('drag', event.position().toPoint())

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.LeftButton and self.pressed is not None:
            point = event.position().toPoint()
            if (point - self.pressed).manhattanLength() < 12:
                self.touch('tap', point)
            self.touch('release', point)
            self.pressed = None

    def refresh(self):
        path = Path('/tmp/glhost_frame.ppm')
        try:
            stamp = path.stat().st_mtime_ns
            if stamp == self.stamp:
                return
            # Validate the complete PPM before reading a concurrently written frame.
            data = path.read_bytes()
            if not data.startswith(b'P6\n800 480\n255\n') or len(data) != 1152015:
                return
            frame = QImage.fromData(data, 'PPM')
            if not frame.isNull():
                self.frame, self.stamp = frame, stamp
                self.update()
        except OSError:
            pass

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), Qt.black)
        if not self.frame.isNull():
            painter.drawImage(self.rect(), self.frame)


class RotaryButton(QPushButton):
    """A pressable physical knob; mouse wheel sends signed encoder ticks."""
    def __init__(self, text, code, panel):
        super().__init__(text)
        self.setObjectName('knob')
        self.setFixedSize(86, 86)
        self.code, self.panel = code, panel
        self.wheel_remainder = 0
        self.setToolTip('Click to press; scroll to turn')
        connect_key(self, code, panel)

    def wheelEvent(self, event):
        self.wheel_remainder += event.angleDelta().y()
        ticks = int(self.wheel_remainder / 120)
        self.wheel_remainder -= ticks * 120
        if ticks:
            self.panel.send(dict(type='encoder', code=self.code, ticks=max(-32, min(32, ticks))))
        event.accept()


def connect_key(button, code, panel):
    button.pressed.connect(lambda: panel.send(dict(type='key', code=code, pressed=1)))
    button.released.connect(lambda: panel.send(dict(type='key', code=code, pressed=0)))


def control_column(keys, knob_text, knob_code, panel):
    column = QVBoxLayout()
    column.setSpacing(4)
    for name, code in keys:
        button = QPushButton(name)
        button.setObjectName(name.lower())
        button.setFixedSize(100, 65)
        connect_key(button, code, panel)
        column.addWidget(button)
    column.addStretch()
    if knob_code is None:
        return column
    column.addWidget(RotaryButton(knob_text, knob_code, panel), alignment=Qt.AlignHCenter)
    turns = QHBoxLayout()
    for text, ticks in [('−', -1), ('+', 1)]:
        button = QPushButton(text)
        button.setObjectName(f'encoder_{knob_code}_{ticks}')
        button.setFixedSize(48, 32)
        button.setToolTip('Turn one step')
        button.clicked.connect(lambda checked=False, ticks=ticks: panel.send(dict(type='encoder', code=knob_code, ticks=ticks)))
        turns.addWidget(button)
    column.addLayout(turns)
    return column


class ClusterWindow(QWidget):
    def __init__(self, parent, send, porsche=False):
        super().__init__(parent, Qt.Window)
        self.source=os.environ.get('MHI2_CLUSTER_SOURCE','MOST video')
        self.setWindowTitle(('Porsche cluster — ' if porsche else 'Virtual Cockpit — ')+self.source)
        layout=QVBoxLayout(self)
        self.screen=QLabel('Waiting for '+self.source)
        self.screen.setAlignment(Qt.AlignCenter)
        if porsche:
            self.screen.setFixedSize(408,448)
        else:
            self.screen.setMinimumSize(800,400)
        layout.addWidget(self.screen)
        self.status=QLabel('No video received')
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        if porsche:
            note=QLabel('Porsche map viewport · 408 × 448\nPhysical 718 output remains unverified.')
            note.setWordWrap(True)
            note.setMaximumWidth(408)
            layout.addWidget(note)
        self.timer=QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(100)
        self.stamp=None
        self.send=send
        self.requested=float('-inf')
        self.auto_request=not porsche and 'MHI2_CLUSTER_SOURCE' not in os.environ
        self.path=Path(os.environ.get('MHI2_CLUSTER_FRAME','/tmp/mhi2-cluster.ppm'))

    def showEvent(self, event):
        super().showEvent(event)
        self.requested=float('-inf')
        self.refresh()

    def request_stream(self):
        self.send({'type':'cluster-map'})
        self.send({'type':'cluster', 'context':70})
        self.requested=time.monotonic()

    def refresh(self):
        path=self.path
        fresh=False
        try:
            info=path.stat()
            fresh=time.time()-info.st_mtime<10
            self.status.setText(('Receiving '+self.source) if fresh else 'Video paused — last received frame')
            stamp=info.st_mtime_ns
            if self.auto_request and self.isVisible() and not fresh and time.monotonic()-self.requested>15:
                self.request_stream()
            if stamp==self.stamp:return
            frame=QImage.fromData(path.read_bytes(),'PPM')
            if frame.isNull():return
            self.screen.setPixmap(QPixmap.fromImage(frame).scaled(self.screen.size(),Qt.KeepAspectRatio,Qt.SmoothTransformation))
            self.stamp=stamp
        except OSError:
            self.status.setText('No video received')
            if self.stamp is not None:
                self.screen.setText('Waiting for '+self.source)
                self.stamp=None
            if self.auto_request and self.isVisible() and time.monotonic()-self.requested>15:
                self.request_stream()


def porsche_controls(panel):
    controls = QVBoxLayout()
    transport = QHBoxLayout()
    for label, code in [('◀◀', 10), ('▶▶', 14), ('OPT', 106), ('BACK', 13)]:
        if label == 'OPT':
            transport.addStretch()
        button = QPushButton(label)
        button.setObjectName({10: 'previous', 14: 'next', 106: 'opt', 13: 'back'}[code])
        button.setFixedSize(92, 32)
        connect_key(button, code, panel)
        transport.addWidget(button)
    controls.addLayout(transport)
    row = QHBoxLayout()
    row.addWidget(RotaryButton('POWER\nVOLUME', 17, panel))
    row.addStretch()
    for keys in PORSCHE_KEYS:
        column = QVBoxLayout()
        for label, code in keys:
            button = QPushButton(label)
            button.setObjectName(label.lower())
            button.setFixedSize(130, 40)
            connect_key(button, code, panel)
            column.addWidget(button)
        row.addLayout(column)
    row.addStretch()
    row.addWidget(RotaryButton('SELECT\nTUNE', 16, panel))
    controls.addLayout(row)
    return controls


def create_window(profile=None):
    profile = current() if profile is None else profile
    porsche = profile['brand'] == 'porsche'
    window = QWidget()
    window.setWindowTitle(profile['title'])
    window.setStyleSheet('''
        QWidget { background: #202124; color: #eeeeee; }
        QPushButton { background: #34363a; border: 1px solid #65676b;
                      border-radius: 5px; font-size: 13px; font-weight: bold; }
        QPushButton:hover { background: #494c51; border-color: #bfc2c7; }
        QPushButton:pressed { background: #125780; border-color: #52b7f2; }
        QPushButton#knob { border: 4px solid #a3a6aa; border-radius: 43px; }
        QLabel { color: #bfc2c7; font-size: 12px; }
    ''')
    if porsche:
        window.setStyleSheet(window.styleSheet() + '''
            QWidget { background: #191919; }
            QPushButton { background: #282828; border-radius: 2px; }
            QPushButton:pressed { background: #9e4612; border-color: #f18b35; }
            QLabel#brand { color: #eeeeee; font-size: 17px; letter-spacing: 5px; }
        ''')
    layout = QVBoxLayout(window)
    if porsche:
        brand = QLabel('PORSCHE')
        brand.setObjectName('brand')
        brand.setAlignment(Qt.AlignCenter)
        layout.addWidget(brand)
    status = QLabel('Touch the screen or use the buttons. Scroll over a knob to turn it.')
    panel = Panel(status, profile)
    fascia = QHBoxLayout()
    fascia.setSpacing(14)
    if profile['touch'] and not porsche:
        fascia.addLayout(control_column(LEFT_KEYS, 'POWER\nVOLUME', 17, panel))
    fascia.addWidget(panel)
    if profile['touch'] and not porsche:
        fascia.addLayout(control_column(RIGHT_KEYS, 'SELECT\nTUNE', 16, panel))
    layout.addLayout(fascia)
    if porsche:
        layout.addLayout(porsche_controls(panel))
    if not profile['touch']:
        status.setText('Use the MMI controller: turn to browse, press to select. Arrow keys, Enter and Esc also work.')
        shortcuts = QHBoxLayout()
        for name, code in [('RADIO',15), ('MEDIA',1), ('NAV',4), ('TEL',3), ('CAR',6), ('MENU',78)]:
            button = QPushButton(name)
            button.setObjectName(name.lower())
            button.setMinimumHeight(42)
            connect_key(button, code, panel)
            shortcuts.addWidget(button)
        layout.addLayout(shortcuts)
        controller = QHBoxLayout()
        controller.addLayout(control_column([('TOP\nLEFT',8), ('BOTTOM\nLEFT',9)], 'VOLUME',17,panel))
        controller.addStretch()
        controller.addLayout(control_column([], 'SELECT',16,panel))
        controller.addStretch()
        controller.addLayout(control_column([('TOP\nRIGHT',11), ('BOTTOM\nRIGHT',12), ('BACK',13)], None,None,panel))
        layout.addLayout(controller)
        def tap(code):
            panel.send(dict(type='key',code=code,pressed=1))
            QTimer.singleShot(60, lambda: panel.send(dict(type='key',code=code,pressed=0)))
        window.input_shortcuts = []
        for key, ticks in [('Left',-1), ('Up',-1), ('Right',1), ('Down',1)]:
            shortcut = QShortcut(QKeySequence(key),window)
            shortcut.activated.connect(lambda ticks=ticks: panel.send(dict(type='encoder',code=16,ticks=ticks)))
            window.input_shortcuts.append(shortcut)
        for key, code in [('Return',16), ('Enter',16), ('Escape',13), ('M',78)]:
            shortcut = QShortcut(QKeySequence(key),window)
            shortcut.activated.connect(lambda code=code: tap(code))
            window.input_shortcuts.append(shortcut)
    footer = QHBoxLayout()
    back = QPushButton('BACK')
    back.setFixedSize(100, 32)
    back.setToolTip('Additional back control')
    connect_key(back, 13, panel)
    if profile['touch'] and not porsche:
        footer.addWidget(back)
    footer.addStretch()
    settings = QPushButton('Configuration')
    settings.setFixedSize(150, 32)
    def configure():
        from config_dialog import ConfigDialog
        try:
            dialog = ConfigDialog(window)
            if dialog.exec():
                status.setText('Configuration saved. Applies on the next emulator start.')
        except (OSError, ValueError) as exc:
            QMessageBox.warning(window, 'Configuration unavailable', str(exc))
    settings.clicked.connect(configure)
    footer.addWidget(settings)
    cluster=ClusterWindow(window,panel.send,porsche=porsche)
    window.cluster=cluster
    cluster_button=QPushButton("Cluster display" if porsche else "Virtual Cockpit")
    cluster_button.setObjectName('cluster_display')
    cluster_button.setFixedSize(150,32)
    cluster_button.clicked.connect(cluster.show)
    footer.addWidget(cluster_button)
    if profile['release']:
        footer.addWidget(QLabel(profile['release']))
    layout.addLayout(footer)
    layout.addWidget(status)
    window.setFixedSize(window.sizeHint())
    return window


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware-meta', type=Path)
    parser.add_argument('--brand', choices=('auto', 'porsche', 'volkswagen', 'audi'), default='auto')
    args, qt_args = parser.parse_known_args()
    if args.firmware_meta:
        os.environ['MHI2_FIRMWARE_META'] = str(args.firmware_meta)
    app = QApplication([sys.argv[0]] + qt_args)
    window = create_window(current() if args.brand == 'auto' else
                           select_profile(common_metadata(args.firmware_meta), args.brand))
    window.setAttribute(Qt.WA_DeleteOnClose)
    window.show()
    if os.environ.get('MHI2_SHOW_CLUSTER') == '1':
        window.cluster.show()
    return app.exec()


if __name__ == '__main__':
    sys.exit(main())

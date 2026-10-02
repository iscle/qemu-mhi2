#!/usr/bin/env python3
"""Display experimental GLES frames and send production RCC keypanel events."""
import json
import os
import sys
import time
from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QImage, QPainter, QPixmap, QShortcut, QKeySequence
from PySide6.QtWidgets import QApplication, QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QLabel

from firmware_profile import current

# Firmware org.dsi.ifc.keypanel.Constants; profile selects ABT/FCC identity.
LEFT_KEYS = [('RADIO', 15), ('MEDIA', 1), ('PHONE', 3), ('VOICE', 50)]
RIGHT_KEYS = [('NAV', 4), ('TRAFFIC', 5), ('CAR', 6), ('MENU', 78)]


class Panel(QWidget):
    def __init__(self, status):
        super().__init__()
        self.setFixedSize(800, 480)
        self.status = status
        self.touch_enabled = current()["touch"]
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
    def __init__(self, parent, send):
        super().__init__(parent, Qt.Window)
        self.setWindowTitle('Virtual Cockpit — MOST video')
        layout=QVBoxLayout(self)
        self.screen=QLabel('Waiting for firmware video over MOST')
        self.screen.setAlignment(Qt.AlignCenter)
        self.screen.setMinimumSize(800,400)
        layout.addWidget(self.screen)
        self.timer=QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(100)
        self.stamp=None
        self.send=send
        self.requested=0

    def showEvent(self, event):
        super().showEvent(event)
        self.requested=0
        self.refresh()

    def request_stream(self):
        self.send({'type':'cluster-map'})
        self.send({'type':'cluster', 'context':70})
        self.requested=time.monotonic()

    def refresh(self):
        path=Path('/tmp/mhi2-cluster.ppm')
        fresh=False
        try:
            info=path.stat()
            fresh=time.time()-info.st_mtime<10
            stamp=info.st_mtime_ns
            if self.isVisible() and not fresh and time.monotonic()-self.requested>15:
                self.request_stream()
            if stamp==self.stamp:return
            frame=QImage.fromData(path.read_bytes(),'PPM')
            if frame.isNull():return
            self.screen.setPixmap(QPixmap.fromImage(frame).scaled(self.screen.size(),Qt.KeepAspectRatio,Qt.SmoothTransformation))
            self.stamp=stamp
        except OSError:
            if self.isVisible() and time.monotonic()-self.requested>15:
                self.request_stream()


def create_window():
    window = QWidget()
    profile = current()
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
    layout = QVBoxLayout(window)
    status = QLabel('Touch the screen or use the side buttons. Scroll over a knob to turn it.'
                   if profile['touch'] else
                   'Use the MMI controller: turn to browse, press to select. Arrow keys, Enter and Esc also work.')
    panel = Panel(status)
    fascia = QHBoxLayout()
    fascia.setSpacing(14)
    if profile['touch']:
        fascia.addLayout(control_column(LEFT_KEYS, 'POWER\nVOLUME', 17, panel))
        fascia.addWidget(panel)
        fascia.addLayout(control_column(RIGHT_KEYS, 'SELECT\nTUNE', 16, panel))
        layout.addLayout(fascia)
    else:
        layout.addWidget(panel)
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
    if profile['touch']:
        back = QPushButton('BACK')
        back.setFixedSize(100, 32)
        back.setToolTip('Additional back control')
        connect_key(back, 13, panel)
        footer.addWidget(back)
    footer.addStretch()
    cluster=ClusterWindow(window,panel.send)
    window.cluster=cluster
    cluster_button=QPushButton("Virtual Cockpit")
    cluster_button.setFixedSize(150,32)
    cluster_button.clicked.connect(cluster.show)
    footer.addWidget(cluster_button)
    layout.addLayout(footer)
    layout.addWidget(status)
    window.setFixedSize(window.sizeHint())
    return window


def main():
    app = QApplication(sys.argv)
    window = create_window()
    window.setAttribute(Qt.WA_DeleteOnClose)
    window.show()
    return app.exec()


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Display experimental GLES frames and send production RCC keypanel events."""
import json
import argparse
import os
import sys
import time
from pathlib import Path

from PySide6.QtCore import Qt, QTimer, QPoint, QPointF
from PySide6.QtGui import QImage, QShortcut, QKeySequence, QPainter, QPainterPath, QPen, QColor
from PySide6.QtWidgets import QApplication, QWidget, QVBoxLayout, QHBoxLayout, QGridLayout, QPushButton, QLabel, QMessageBox
from firmware_profile import common_metadata, select_profile

from firmware_profile import current
from viewer_widgets import FrameView, fit_to_screen
from mmi_touchpad import STATE_PATH, CURSOR_PATH

# Firmware org.dsi.ifc.keypanel.Constants; profile selects ABT/FCC identity.
LEFT_KEYS = [('RADIO', 15), ('MEDIA', 1), ('PHONE', 3), ('VOICE', 50)]
RIGHT_KEYS = [('NAV', 4), ('TRAFFIC', 5), ('CAR', 6), ('MENU', 78)]
# Porsche PCM 4 controls. K5126's active key mapping opens Home on MENU=78;
# the generic keypanel HOME=116 is ignored. Layout: MY18 PCM quick reference.
PORSCHE_KEYS = [(('TUNER', 15), ('SOURCE', 103)),
                (('MEDIA', 1), ('PHONE', 3)),
                (('NAV', 4), ('CAR', 6)),
                (('MAP', 104), ('HOME', 78))]


class Panel(FrameView):
    def __init__(self, status, profile=None):
        active = current() if profile is None else profile
        self.screen_size = tuple(active.get('screen', (800, 480)))  # A4 MMI: 1024x480
        super().__init__(*self.screen_size)
        self.status = status
        self.touch_enabled = active["touch"]
        self.frame = QImage()
        self.stamp = None
        self.pressed = None
        self.map_cursor = None
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

    def guest_point(self, point):
        rect = self.frame_rect()
        return QPoint(max(0, min(self.screen_size[0] - 1, int((point.x() - rect.x()) * self.screen_size[0] / rect.width()))),
                      max(0, min(self.screen_size[1] - 1, int((point.y() - rect.y()) * self.screen_size[1] / rect.height()))))

    def mousePressEvent(self, event):
        if (self.touch_enabled and event.button() == Qt.LeftButton and
                self.frame_rect().contains(event.position().toPoint())):
            self.pressed = self.guest_point(event.position())
            self.touch('press', self.pressed)

    def mouseMoveEvent(self, event):
        if self.pressed is not None:
            self.touch('drag', self.guest_point(event.position()))

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.LeftButton and self.pressed is not None:
            point = self.guest_point(event.position())
            if (point - self.pressed).manhattanLength() < 12:
                self.touch('tap', point)
            self.touch('release', point)
            self.pressed = None

    def refresh(self):
        cursor = None
        try:
            state = json.loads(STATE_PATH.read_text())
            point = json.loads(CURSOR_PATH.read_text())
            if (state['ready'] and state['mode'] == 25 and time.time()-state['updated'] < 2
                    and point['visible'] and point['generation'] == state['generation']):
                cursor = (point['x'], point['y'])
        except (OSError, ValueError, KeyError, TypeError):
            pass
        if cursor != self.map_cursor:
            self.map_cursor = cursor
            self.update()
        path = Path('/tmp/glhost_frame.ppm')
        try:
            stamp = path.stat().st_mtime_ns
            if stamp == self.stamp:
                return
            # Validate the complete PPM before reading a concurrently written frame.
            data = path.read_bytes()
            header = 'P6\n%d %d\n255\n' % self.screen_size
            if not data.startswith(header.encode()) or len(data) != len(header) + self.screen_size[0] * self.screen_size[1] * 3:
                return
            frame = QImage.fromData(data, 'PPM')
            if not frame.isNull():
                self.frame, self.stamp = frame, stamp
                self.update()
        except OSError:
            pass

    def paintEvent(self, event):
        super().paintEvent(event)
        if self.map_cursor is None or self.frame.isNull():
            return
        rect = self.frame_rect()
        scale = rect.width()/self.screen_size[0]
        x = rect.x()+self.map_cursor[0]*scale
        y = rect.y()+self.map_cursor[1]*scale
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        for color, width in [('#111111', 4), ('#ffffff', 2)]:
            painter.setPen(QPen(QColor(color), width*scale))
            painter.drawEllipse(QPointF(x,y), 7*scale, 7*scale)
            for a,b,c,d in [(-17,0,-9,0),(9,0,17,0),(0,-17,0,-9),(0,9,0,17)]:
                painter.drawLine(QPointF(x+a*scale,y+b*scale),QPointF(x+c*scale,y+d*scale))

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
        self.screen=FrameView(*((408,448) if porsche else (800,400)),
                              message='Waiting for '+self.source)
        layout.addWidget(self.screen, 1)
        self.status=QLabel('No video received')
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        if porsche:
            note=QLabel('Porsche map viewport · 408 × 448\nPhysical 718 output remains unverified.')
            note.setWordWrap(True)
            layout.addWidget(note)
        self.timer=QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(100)
        self.stamp=None
        self.send=send
        self.requested=float('-inf')
        self.auto_request=not porsche and 'MHI2_CLUSTER_SOURCE' not in os.environ
        self.path=Path(os.environ.get('MHI2_CLUSTER_FRAME','/tmp/mhi2-cluster.ppm'))
        fit_to_screen(self)

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
            self.screen.frame=frame
            self.screen.update()
            self.stamp=stamp
        except OSError:
            self.status.setText('No video received')
            if self.stamp is not None:
                self.screen.frame=QImage()
                self.screen.update()
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
            button.setMinimumSize(64, 32)
            button.setMaximumHeight(40)
            connect_key(button, code, panel)
            column.addWidget(button)
        row.addLayout(column)
    row.addStretch()
    row.addWidget(RotaryButton('SELECT\nTUNE', 16, panel))
    controls.addLayout(row)
    return controls


class RotaryArrowButton(QPushButton):
    """Curved encoder control framing the MMI Select knob."""
    def __init__(self, ticks, panel):
        super().__init__()
        self.ticks = ticks
        self.setObjectName(f'encoder_16_{ticks}')
        self.setFixedSize(44, 160)
        direction = 'clockwise' if ticks > 0 else 'counterclockwise'
        self.setAccessibleName('Turn MMI Select ' + direction)
        self.setToolTip('Turn MMI Select ' + direction + ' one step')
        self.clicked.connect(lambda: panel.send(dict(type='encoder', code=16, ticks=ticks)))

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        color = '#e56d74' if self.isDown() else '#ffffff' if self.underMouse() else '#c1c4c7'
        painter.setPen(QPen(QColor(color), 3, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
        # Left arc points upward (CW); right arc points upward (CCW).
        left = self.ticks > 0
        x = 36 if left else 8
        bend = -5 if left else 49
        path = QPainterPath(QPointF(x, 16))
        path.cubicTo(bend, 48, bend, 112, x, 144)
        painter.drawPath(path)
        painter.drawLine(QPointF(x,16), QPointF(x + (-12 if left else 12),19))
        painter.drawLine(QPointF(x,16), QPointF(x - (1 if left else -1),29))
        if self.hasFocus():
            painter.setPen(QPen(QColor('#ffffff'), 1, Qt.DotLine))
            painter.drawRoundedRect(self.rect().adjusted(1,1,-1,-1), 8, 8)


def audi_controls(panel):
    """Audi MMI fascia; rocker halves retain the existing RCC key codes."""
    housing = QWidget()
    housing.setObjectName('audi_controller')
    housing.setStyleSheet("""
        QWidget#audi_controller { background: #242526; border: 2px solid #111213;
                                  border-radius: 24px; }
        QPushButton { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
                      stop:0 #383a3c,stop:1 #242527); border: 1px solid #56585b;
                      border-radius: 9px; color: #f0f0f0; font-size: 14px; }
        QPushButton:hover { border-color: #c8cace; background: #414345; }
        QPushButton:focus { border-color: #f0f0f0; }
        QPushButton:pressed { background: #701d24; border-color: #e56d74; }
        QPushButton[rocker="true"] { color: #202124; border-radius: 5px;
            background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
            stop:0 #f1f2f3,stop:0.45 #bfc2c5,stop:0.55 #777c82,stop:1 #d9dcdf); }
        QPushButton[rocker="true"]:hover { border-color: white; }
        QPushButton[rocker="true"]:pressed { background: #8c9298; }
        QPushButton#knob { border: 7px solid #c1c4c7; border-radius: 70px;
            background: qradialgradient(cx:0.4,cy:0.3,radius:0.9,
            stop:0 #383a3b,stop:1 #171819); font-size: 16px; }
        QPushButton#knob:focus { border-color: white; }
        QPushButton#knob:pressed { background: #701d24; }
    """)
    row = QHBoxLayout(housing)
    row.setContentsMargins(24, 16, 24, 16)
    row.setSpacing(20)

    def button(label, name, code, height=44):
        key = QPushButton(label)
        key.setObjectName(name)
        key.setMinimumSize(100, height)
        connect_key(key, code, panel)
        return key

    def rocker(upper, lower):
        column = QVBoxLayout()
        column.setSpacing(0)
        for label, name, code, arrow in [(*upper, '▲'), (*lower, '▼')]:
            key = button(arrow + '  ' + label, name, code)
            key.setProperty('rocker', True)
            key.setToolTip('Rocker up: ' + label if arrow == '▲' else 'Rocker down: ' + label)
            column.addWidget(key)
        return column

    left = QVBoxLayout()
    left.addLayout(rocker(('NAV/MAP', 'nav', 4), ('TEL', 'tel', 3)))
    left.addWidget(button('LEFT', 'left', 98, height=64))
    row.addLayout(left)
    center = QVBoxLayout()
    top = QHBoxLayout()
    top.addWidget(button('MENU', 'menu', 78))
    top.addWidget(button('↶  BACK', 'back', 13))
    center.addLayout(top)
    knob = RotaryButton('MMI\nSELECT', 16, panel)
    knob.setFixedSize(140, 140)
    joystick = QGridLayout()
    joystick.setSpacing(3)
    joystick.addWidget(knob, 1, 1)
    for direction, label, y, x in [('up', '▲', 0, 1), ('left', '◀', 1, 0),
                                    ('right', '▶', 1, 2), ('down', '▼', 2, 1)]:
        key = QPushButton(label)
        key.setObjectName('joystick_' + direction)
        key.setFixedSize(36, 36)
        key.setAccessibleName('MMI Select joystick ' + direction)
        key.setToolTip('Hold to move MMI Select ' + direction)
        key.pressed.connect(lambda direction=direction:
                            panel.send(dict(type='joystick', direction=direction)))
        key.released.connect(lambda: panel.send(dict(type='joystick', direction='neutral')))
        joystick.addWidget(key, y, x, Qt.AlignCenter)
    turns = QHBoxLayout()
    turns.setSpacing(2)
    turns.addWidget(RotaryArrowButton(1, panel))
    turns.addLayout(joystick)
    turns.addWidget(RotaryArrowButton(-1, panel))
    center.addLayout(turns)
    row.addLayout(center)
    right = QVBoxLayout()
    right.addLayout(rocker(('RADIO', 'radio', 15), ('MEDIA', 'media', 1)))
    right.addWidget(button('RIGHT', 'right', 97, height=64))
    row.addLayout(right)
    auxiliary = QHBoxLayout()
    auxiliary.addStretch()
    if panel.screen_size == (1024, 480):
        from mmi_touch_widget import TouchpadWidget
        auxiliary.addWidget(TouchpadWidget(panel))
        auxiliary.addSpacing(12)
    extras = QVBoxLayout()
    for label, name, code in [('CAR', 'car', 6), ('VOICE', 'voice', 50), ('TRAFFIC', 'traffic', 5)]:
        extras.addWidget(button(label, name, code))
    auxiliary.addLayout(extras)
    auxiliary.addSpacing(20)
    auxiliary.addWidget(housing)
    auxiliary.addSpacing(20)
    volume = QVBoxLayout()
    volume.setSpacing(8)
    volume.addStretch()
    volume.addWidget(RotaryButton('POWER\nVOLUME', 17, panel), alignment=Qt.AlignHCenter)
    volume_turns = QHBoxLayout()
    for label, ticks in [('−', -1), ('+', 1)]:
        key = QPushButton(label)
        key.setObjectName(f'encoder_17_{ticks}')
        key.setMinimumSize(44, 44)
        key.setToolTip('Volume down one step' if ticks < 0 else 'Volume up one step')
        key.clicked.connect(lambda checked=False, ticks=ticks: panel.send(dict(type='encoder', code=17, ticks=ticks)))
        volume_turns.addWidget(key)
    volume.addLayout(volume_turns)
    volume.addStretch()
    auxiliary.addLayout(volume)
    auxiliary.addStretch()
    return auxiliary


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
    status.setWordWrap(True)
    panel = Panel(status, profile)
    fascia = QHBoxLayout()
    fascia.setSpacing(14)
    if profile['touch'] and not porsche:
        fascia.addLayout(control_column(LEFT_KEYS, 'POWER\nVOLUME', 17, panel))
    fascia.addWidget(panel, 1)
    if profile['touch'] and not porsche:
        fascia.addLayout(control_column(RIGHT_KEYS, 'SELECT\nTUNE', 16, panel))
    layout.addLayout(fascia, 1)
    if porsche:
        layout.addLayout(porsche_controls(panel))
    if not profile['touch']:
        status.setText('Use the MMI controller: turn to browse, press to select. Arrow keys, Enter and Esc also work.')
        layout.addLayout(audi_controls(panel))
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
    fit_to_screen(window)
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

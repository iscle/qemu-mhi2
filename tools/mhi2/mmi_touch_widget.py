"""Mouse-operated MMI touch surface, separate from the rotary controller."""
import json
import time
from PySide6.QtCore import Qt, QTimer, Signal, QPointF
from PySide6.QtGui import QPainter, QPen, QColor
from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QLabel, QPushButton, QComboBox
from mmi_touchpad import STATE_PATH, CHARACTER_MODES, MAP_MODES, recognize


class InkSurface(QWidget):
    completed = Signal()

    def __init__(self, owner):
        super().__init__(owner)
        self.owner = owner
        self.strokes = []
        self.drawing = False
        self.sent = False
        self.last_move = 0
        self.map_stroke = False
        self.setObjectName('mmi_ink')
        self.setFixedSize(190, 170)
        self.setAccessibleName('MMI handwriting surface')
        self.setToolTip('Hold the left mouse button to draw a digit or block letter')

    def point(self, event):
        return (max(10, min(self.width()-10, event.position().x())),
                max(10, min(self.height()-10, event.position().y())))

    def send_point(self, action, point):
        if not self.owner.active:
            return
        x, y = point
        self.owner.panel.send(dict(type='touchpad', action=action,
                                   x=max(1, round((x-10)*1000/(self.width()-20))),
                                   y=max(1, round((y-10)*1000/(self.height()-20))),
                                   generation=self.owner.generation,
                                   input_mode='map' if self.map_stroke else 'characters'))

    def mousePressEvent(self, event):
        if event.button() != Qt.LeftButton:
            return
        if self.sent:
            self.owner.clear()
        self.owner.finish_timer.stop()
        self.drawing = True
        self.map_stroke = self.owner.map_mode
        point = self.point(event)
        self.strokes.append([point])
        if self.map_stroke:
            self.strokes = [[point]]
        self.owner.send_button.setEnabled(False)
        self.send_point('press', point)
        self.update()

    def mouseMoveEvent(self, event):
        if not self.drawing:
            return
        point = self.point(event)
        if point == self.strokes[-1][-1]:
            return
        self.strokes[-1].append(point)
        now = time.monotonic()
        if now-self.last_move >= 0.025:
            self.send_point('move', point)
            self.last_move = now
        self.update()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.LeftButton and self.drawing:
            point = self.point(event)
            if self.map_stroke and point != self.strokes[-1][-1]:
                self.send_point('move', point)
            self.strokes[-1].append(point)
            self.send_point('release', point)
            self.drawing = False
            self.completed.emit()
            self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        painter.setBrush(QColor('#141516'))
        painter.setPen(QPen(QColor('#777b80'), 1))
        painter.drawRoundedRect(self.rect().adjusted(1,1,-1,-1), 12, 12)
        if not self.strokes:
            painter.setPen(QColor('#c4c7cb'))
            hint = 'Drag the cursor\nMMI Select to choose' if self.owner.map_mode else 'Draw here\n' + ('0–9 / А–Я' if self.owner.alphabet.currentData() == 'cyrillic' else '0–9 / A–Z')
            painter.drawText(self.rect(), Qt.AlignCenter, hint)
        painter.setPen(QPen(QColor('#f1f2f3'), 4, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
        for stroke in self.strokes:
            if stroke:
                painter.drawPoint(QPointF(*stroke[0]))
            for a, b in zip(stroke, stroke[1:]):
                painter.drawLine(QPointF(*a), QPointF(*b))


class TouchpadWidget(QWidget):
    def __init__(self, panel):
        super().__init__()
        self.panel = panel
        self.active = False
        self.generation = None
        self.setObjectName('mmi_touchpad')
        self.setFixedWidth(200)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4,0,4,0)
        title = QLabel('MMI TOUCH')
        title.setAlignment(Qt.AlignCenter)
        layout.addWidget(title)
        self.input_mode = QComboBox()
        self.input_mode.setObjectName('mmi_touch_mode')
        self.input_mode.setAccessibleName('MMI Touch mode')
        self.input_mode.addItem('Map', 'map')
        self.input_mode.addItem('Characters', 'characters')
        layout.addWidget(self.input_mode)
        self.alphabet = QComboBox()
        self.alphabet.setAccessibleName('Handwriting alphabet')
        self.alphabet.addItem('Cyrillic (А–Я)', 'cyrillic')
        self.alphabet.addItem('Latin (A–Z)', 'latin')
        layout.addWidget(self.alphabet)
        self.surface = InkSurface(self)
        layout.addWidget(self.surface, alignment=Qt.AlignHCenter)
        self.candidates = QComboBox()
        self.candidates.setAccessibleName('Recognized character candidates')
        layout.addWidget(self.candidates)
        buttons = QHBoxLayout()
        self.send_button = QPushButton('Send')
        self.send_button.setMinimumHeight(36)
        self.send_button.setEnabled(False)
        self.send_button.clicked.connect(self.send_character)
        buttons.addWidget(self.send_button)
        clear = QPushButton('Clear')
        clear.setMinimumHeight(36)
        clear.clicked.connect(self.clear)
        buttons.addWidget(clear)
        layout.addLayout(buttons)
        self.note = QLabel('Open an MMI text field')
        self.note.setWordWrap(True)
        layout.addWidget(self.note)
        self.finish_timer = QTimer(self)
        self.finish_timer.setSingleShot(True)
        self.finish_timer.setInterval(850)
        self.finish_timer.timeout.connect(self.finish)
        self.surface.completed.connect(self.stroke_completed)
        self.timer = QTimer(self)
        self.timer.setInterval(300)
        self.timer.timeout.connect(self.refresh_state)
        self.timer.start()
        self.refresh_state()
        self.alphabet.currentIndexChanged.connect(self.clear)
        self.input_mode.currentIndexChanged.connect(self.mode_changed)
        self.mode_changed()

    @property
    def map_mode(self):
        return self.input_mode.currentData() == 'map'

    def stroke_completed(self):
        if self.map_mode:
            self.surface.strokes.clear()
            self.surface.update()
        else:
            self.finish_timer.start()

    def mode_changed(self):
        self.clear()
        self.active = False
        self.surface.setAccessibleName('MMI map touch surface' if self.map_mode else 'MMI handwriting surface')
        self.surface.setToolTip('Drag to move the cursor; use MMI Select to choose' if self.map_mode else 'Draw a digit or block letter')
        for widget in (self.alphabet, self.candidates, self.send_button):
            widget.setVisible(not self.map_mode)
        self.refresh_state()

    def refresh_state(self):
        try:
            state = json.loads(STATE_PATH.read_text())
            ready = (state['enabled'] and state['ready'] and time.time()-state['updated'] < 2)
            active = ready and state['mode'] in (MAP_MODES if self.map_mode else CHARACTER_MODES)
            generation = state['generation']
        except (OSError, ValueError, KeyError, TypeError):
            state, active, generation = {}, False, None
        if generation != self.generation or active != self.active:
            self.clear()
            self.generation, self.active = generation, active
            self.note.setText(('Drag / tap on the map' if active else 'Open the navigation map') if self.map_mode else ('Draw a digit or block letter' if active else 'Open an MMI text field'))
        if state.get('error'):
            self.note.setText(state['error'])
        self.send_button.setEnabled(self.active and self.candidates.count() > 0 and not self.surface.sent)

    def clear(self):
        if self.surface.drawing and self.surface.strokes:
            self.surface.send_point('release', self.surface.strokes[-1][-1])
        self.finish_timer.stop()
        self.surface.strokes.clear()
        self.surface.drawing = self.surface.sent = False
        self.candidates.clear()
        self.send_button.setEnabled(False)
        self.surface.update()

    def finish(self):
        if self.map_mode:
            return
        self.refresh_state()
        if not self.surface.strokes:
            return
        # A click or minute accidental mark is not a glyph.
        points = [p for stroke in self.surface.strokes for p in stroke]
        span = max(max(p[i] for p in points)-min(p[i] for p in points) for i in (0,1))
        results = recognize(self.surface.strokes, alphabet=self.alphabet.currentData()) if span >= 18 else []
        self.candidates.clear()
        for char, similarity in results:
            self.candidates.addItem(char, char)
        self.send_button.setEnabled(self.active and bool(results))
        if not results:
            self.note.setText('No clear symbol. Clear and try again.')
        elif not self.active:
            self.note.setText('Open an MMI text field to send')
        else:
            # Selection is explicit: similar shapes such as 0/O and 1/I remain candidates.
            self.note.setText('Check the symbol, then Send')

    def send_character(self):
        self.refresh_state()
        character = self.candidates.currentData()
        if not self.active or not character or self.surface.sent:
            return
        self.panel.send(dict(type='touchpad', action='character', character=character,
                             generation=self.generation))
        self.surface.sent = True
        self.send_button.setEnabled(False)
        self.note.setText('Sent ' + character + ' · draw the next symbol')

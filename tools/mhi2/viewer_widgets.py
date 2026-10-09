"""Resizable host windows; guest framebuffer dimensions stay unchanged."""
from PySide6.QtCore import Qt, QSize, QRect
from PySide6.QtGui import QImage, QPainter
from PySide6.QtWidgets import QWidget, QSizePolicy


def fit_to_screen(window):
    """Leave room for window decorations and the desktop's dock/taskbar."""
    available = QWidget.screen(window).availableGeometry().size() - QSize(32, 64)
    window.resize(window.sizeHint().boundedTo(available))


class FrameView(QWidget):
    def __init__(self, width, height, message=''):
        super().__init__()
        self.native_size = QSize(width, height)
        self.frame = QImage()
        self.message = message
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.setMinimumSize(160, 96)

    def sizeHint(self):
        return self.native_size

    def frame_rect(self):
        source = self.frame.size() if not self.frame.isNull() else self.native_size
        size = source.scaled(self.size(), Qt.KeepAspectRatio)
        return QRect((self.width() - size.width()) // 2,
                     (self.height() - size.height()) // 2,
                     size.width(), size.height())

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), Qt.black)
        if self.frame.isNull():
            painter.setPen(Qt.white)
            painter.drawText(self.rect(), Qt.AlignCenter | Qt.TextWordWrap, self.message)
        else:
            painter.setRenderHint(QPainter.SmoothPixmapTransform)
            painter.drawImage(self.frame_rect(), self.frame)

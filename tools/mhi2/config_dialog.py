"""Settings shared by the Porsche and Volkswagen viewers."""
from PySide6.QtWidgets import (QDialog, QVBoxLayout, QLabel, QCheckBox,
    QDialogButtonBox, QScrollArea, QWidget, QFormLayout, QComboBox, QLineEdit,
    QSpinBox, QMessageBox, QTabWidget)
from emulator_config import load, save, config_path
from rcc_features import FEATURES
from viewer_widgets import fit_to_screen


class ConfigDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle('Emulator configuration')
        self.setSizeGripEnabled(True)
        self.config = load()
        layout = QVBoxLayout(self)
        note = QLabel('Changes apply on the next emulator start. Feature states are supplied '
                      'by the simulated RCC; firmware support and the corresponding '
                      'hardware or transport are still required.')
        note.setWordWrap(True)
        layout.addWidget(note)
        tabs = QTabWidget()
        layout.addWidget(tabs)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        features = QWidget()
        items = QVBoxLayout(features)
        self.checks = {}
        for code, name in FEATURES.items():
            key = f'{code:08x}'
            check = QCheckBox(f'{name}  ({key})')
            check.setChecked(self.config['features'][key])
            self.checks[key] = check
            items.addWidget(check)
        items.addStretch()
        scroll.setWidget(features)
        tabs.addTab(scroll, 'RCC features')
        usb = QWidget()
        form = QFormLayout(usb)
        self.mode = QComboBox()
        for name, mode in [('Disconnected', 'off'), ('Android Studio AVD bridge', 'avd'),
                           ('Physical USB passthrough', 'host')]:
            self.mode.addItem(name, mode)
        self.mode.setCurrentIndex(self.mode.findData(self.config['usb']['mode']))
        form.addRow('Connection', self.mode)
        self.serial = QLineEdit(self.config['usb']['serial'])
        self.serial.setPlaceholderText('Auto-select the single running AVD')
        form.addRow('ADB serial', self.serial)
        self.port = QSpinBox()
        self.port.setRange(1024, 65535)
        self.port.setValue(self.config['usb']['port'])
        form.addRow('Local TCP port', self.port)
        self.bus = QSpinBox()
        self.bus.setRange(0, 255)
        self.bus.setValue(self.config['usb']['hostbus'])
        form.addRow('Physical USB bus', self.bus)
        self.hostport = QLineEdit(self.config['usb']['hostport'])
        self.hostport.setPlaceholderText('Port path, e.g. 2.1')
        form.addRow('Physical USB port', self.hostport)
        hint = QLabel('AVD: install the full Android Auto app and start its developer '
                      'head-unit server. The bridge carries that server’s byte stream '
                      'through a USB accessory presented to the HU. It does not export '
                      'the AVD’s USB controller. End-to-end projection is still under test.')
        hint.setWordWrap(True)
        form.addRow(hint)
        usb_scroll = QScrollArea()
        usb_scroll.setWidgetResizable(True)
        usb_scroll.setWidget(usb)
        tabs.addTab(usb_scroll, 'Android Auto / USB')
        def update_fields():
            mode = self.mode.currentData()
            self.serial.setEnabled(mode == 'avd')
            self.port.setEnabled(mode == 'avd')
            self.bus.setEnabled(mode == 'host')
            self.hostport.setEnabled(mode == 'host')
        self.mode.currentIndexChanged.connect(update_fields)
        update_fields()
        path = QLabel(str(config_path()))
        path.setWordWrap(True)
        layout.addWidget(path)
        buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.store)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        fit_to_screen(self)

    def store(self):
        config = {**self.config,
                  'features': {key: check.isChecked() for key, check in self.checks.items()},
                  'usb': {'mode': self.mode.currentData(), 'serial': self.serial.text().strip(),
                          'port': self.port.value(), 'hostbus': self.bus.value(),
                          'hostport': self.hostport.text().strip()}}
        try:
            save(config)
        except (OSError, ValueError) as exc:
            QMessageBox.warning(self, 'Configuration not saved', str(exc))
            return
        self.accept()

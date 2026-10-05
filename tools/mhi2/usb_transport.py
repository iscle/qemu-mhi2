"""Prepare explicitly selected USB transports for the MHI2 launcher."""
import os
import re
import shutil
import subprocess
from pathlib import Path
from firmware_profile import common_metadata


class USBTransport:
    def __init__(self, settings):
        self.settings = settings
        self.forward_owned = False
        self.serial = None
        self.adb = None

    def adb_run(self, *args, selected=True):
        command = [self.adb]
        if selected:
            command += ['-s', self.serial]
        return subprocess.run(command + list(args), check=True, text=True,
                              capture_output=True, timeout=15).stdout

    def prepare(self):
        usb = self.settings
        connector = common_metadata().get('release') == 'MHI2_ER_POG11_K5126'
        devices = (['-device', 'usb-mhi2-hub,id=mediahub,bus=usb-bus.0,port=1,ports=3,port-power=on',
                    '-device', 'usb-mhi2-hfc,id=mediahfc,bus=usb-bus.0,port=1.3'] if connector else [])
        phone_port = '1.1' if connector else '1'
        if usb['mode'] == 'off':
            return devices
        if usb['mode'] == 'host':
            return devices + ['-device', f'usb-host,id=phone,bus=usb-bus.0,port={phone_port},'
                    f'hostbus={usb["hostbus"]},hostport={usb["hostport"]}']
        sdk = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Library/Android/sdk'))
        self.adb = shutil.which('adb') or str(sdk / 'platform-tools/adb')
        connected_devices = self.adb_run('devices', selected=False).splitlines()[1:]
        avds = [line.split()[0] for line in connected_devices
                if len(line.split()) >= 2 and line.split()[1] == 'device'
                and line.startswith('emulator-')]
        self.serial = usb['serial']
        if not self.serial:
            if len(avds) != 1:
                raise ValueError('Start one Android Studio AVD, or select its ADB serial '
                                 'in Configuration. Available AVDs: ' + ', '.join(avds))
            self.serial = avds[0]
        if self.serial not in avds:
            raise ValueError('Selected AVD is not connected: ' + self.serial)
        package = self.adb_run('shell', 'dumpsys', 'package', 'com.google.android.projection.gearhead')
        version = re.search(r'versionName=(\S+)', package)
        if not version or 'stub' in version[1].lower():
            raise ValueError('Install/update the full Android Auto app in the AVD from '
                             'Google Play, then enable developer mode and start its '
                             'head-unit server. The current app is absent or a stub.')
        local = 'tcp:' + str(usb['port'])
        existing = [line.split() for line in self.adb_run('forward', '--list', selected=False).splitlines()]
        matches = [item for item in existing if len(item) == 3 and item[1] == local]
        if matches and matches != [[self.serial, local, 'tcp:5277']]:
            raise ValueError('The selected local port is forwarded to another device or service')
        if not matches:
            self.adb_run('forward', '--no-rebind', local, 'tcp:5277')
            self.forward_owned = True
        # Loopback only. Reconnect reattaches the accessory after the AVD's
        # head-unit server restarts; disconnect resets USB and queued bytes.
        return devices + ['-chardev', f'socket,id=androidauto,host=127.0.0.1,port={usb["port"]},'
                'server=off,reconnect-ms=1000',
                '-device', f'usb-android-auto,id=phone,bus=usb-bus.0,port={phone_port},chardev=androidauto']

    def close(self):
        if self.forward_owned:
            self.adb_run('forward', '--remove', 'tcp:' + str(self.settings['port']))
            self.forward_owned = False

"""Persistent local emulator equipment configuration (not physical FEC files)."""
import argparse
import json
import os
import re
import tempfile
from pathlib import Path
from rcc_features import FEATURES
from firmware_profile import select_profile


def config_path():
    if os.environ.get('MHI2_CONFIG'):
        return Path(os.environ['MHI2_CONFIG']).expanduser()
    return Path.home() / '.config/mhi2' / (select_profile()['brand'] + '.json')


def defaults():
    return {'version': 1, 'features': {f'{code:08x}': True for code in FEATURES},
            # Synthetic fixtures, not a registered vehicle/unit identity.
            # The VIN includes its calculated position-9 check digit.
            'identity': {'vin': 'ZZZEMU00XP0000001',
                         'fazit_id': 'EMU-00009.10.2600000001', 'bg': '5F'},
            'usb': {'mode': 'off', 'serial': '', 'port': 5277,
                    'hostbus': 0, 'hostport': ''}}


def validate(value):
    if not isinstance(value, dict) or type(value.get('version')) is not int or value['version'] != 1:
        raise ValueError('Expected emulator configuration version 1')
    if set(value) - {'version', 'features', 'usb', 'identity'}:
        raise ValueError('Unknown configuration section')
    result = defaults()
    identity = value.get('identity', {})
    if not isinstance(identity, dict) or set(identity) - set(result['identity']):
        raise ValueError('Unknown identity setting')
    result['identity'].update(identity)
    identity = result['identity']
    if not isinstance(identity['vin'], str) or not re.fullmatch(r'[A-HJ-NPR-Z0-9]{17}', identity['vin']):
        raise ValueError('VIN must contain 17 uppercase letters/digits, excluding I, O and Q')
    if not isinstance(identity['fazit_id'], str) or not re.fullmatch(r'[A-Z0-9][A-Z0-9.-]{0,63}', identity['fazit_id']):
        raise ValueError('FAZIT ID must contain 1–64 uppercase letters, digits, dots or hyphens')
    # K5126 OnlineRegistrationServiceImpl supplies this constant itself.
    if identity['bg'] != '5F':
        raise ValueError('The infotainment control-unit identifier must be 5F')
    features = value.get('features', {})
    if not isinstance(features, dict):
        raise ValueError('features must be an object')
    for key, enabled in features.items():
        if key not in result['features'] or type(enabled) is not bool:
            raise ValueError('Unknown feature or non-boolean state: ' + str(key))
        result['features'][key] = enabled
    usb = value.get('usb', {})
    if not isinstance(usb, dict) or set(usb) - set(result['usb']):
        raise ValueError('Unknown USB setting')
    result['usb'].update(usb)
    usb = result['usb']
    if usb['mode'] not in ('off', 'avd', 'host'):
        raise ValueError('USB mode must be off, avd, or host')
    if type(usb['port']) is not int or not 1024 <= usb['port'] <= 65535:
        raise ValueError('AVD forwarding port must be 1024–65535')
    if not isinstance(usb['serial'], str) or any(c.isspace() for c in usb['serial']):
        raise ValueError('Invalid ADB serial')
    if type(usb['hostbus']) is not int or not 0 <= usb['hostbus'] <= 255:
        raise ValueError('USB host bus must be 0–255')
    if not isinstance(usb['hostport'], str):
        raise ValueError('USB host port must be a string')
    if usb['hostport'] and not all(n.isdecimal() and 1 <= int(n) <= 255 for n in usb['hostport'].split('.')):
        raise ValueError('USB host port must be a numeric port path, e.g. 2.1')
    if usb['mode'] == 'host' and not usb['hostport']:
        raise ValueError('Select a physical USB port before enabling passthrough')
    return result


def load(path=None):
    path = Path(path) if path else config_path()
    if not path.exists():
        return defaults()
    return validate(json.loads(path.read_text()))


def save(value, path=None):
    value = validate(value)
    path = Path(path) if path else config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--file', type=Path)
    parser.add_argument('--enable', action='append', default=[], metavar='FEC_HEX')
    parser.add_argument('--disable', action='append', default=[], metavar='FEC_HEX')
    parser.add_argument('--vin', help='Local vehicle VIN (does not provision backend access)')
    parser.add_argument('--fazit-id', help='Local HU manufacturing identity')
    args = parser.parse_args()
    config = load(args.file)
    for codes, state in ((args.enable, True), (args.disable, False)):
        for code in codes:
            key = f'{int(code, 16):08x}'
            if key not in config['features']:
                parser.error('Unknown feature: ' + code)
            config['features'][key] = state
    if args.vin is not None:
        config['identity']['vin'] = args.vin
    if args.fazit_id is not None:
        config['identity']['fazit_id'] = args.fazit_id
    if args.enable or args.disable or args.vin is not None or args.fazit_id is not None:
        save(config, args.file)
    print(json.dumps(config, indent=2))


if __name__ == '__main__':
    main()

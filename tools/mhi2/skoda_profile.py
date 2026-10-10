"""Public, host-neutral metadata for experimental SK profile recognition.

This module identifies a firmware train and its UI/input geometry. It does not
build media, certify bootstrap compatibility, or supply vehicle coding.
"""
import json
import re
from pathlib import Path

ID = 'skoda-skg13-p4526'
RELEASE = 'MHI2_ER_SKG13_P4526'
MU = '1440'
PROFILE_PATH = Path(__file__).with_name('profiles') / f'{ID}.json'


def profile_data():
    data = json.loads(PROFILE_PATH.read_text(encoding='utf-8'))
    if data.get('id') != ID or data.get('schema_version') != 1:
        raise ValueError('Invalid experimental Skoda profile definition')
    return data


def read_metadata(path):
    """Read only unambiguous [common] release/MU fields from metainfo2.txt."""
    values = {}
    in_common = seen_common = False
    for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
        line = line.strip()
        if not line or line.startswith(('#', ';')):
            continue
        if line.startswith('['):
            in_common = line.lower() == '[common]'
            if in_common:
                if seen_common:
                    raise ValueError('Duplicate [common] section')
                seen_common = True
            continue
        if in_common:
            match = re.fullmatch(r'(\w+)\s*=\s*"([^"\r\n]*)"', line)
            if match and match[1] in ('release', 'MUVersion'):
                if match[1] in values:
                    raise ValueError('Duplicate Skoda common metadata: ' + match[1])
                values[match[1]] = match[2]
            elif re.match(r'^(release|MUVersion)\b', line):
                raise ValueError('Malformed Skoda common metadata')
    validate_metadata(values)
    return values


def validate_metadata(metadata):
    if metadata.get('release') != RELEASE or metadata.get('MUVersion') != MU:
        raise ValueError('Experimental Skoda profile requires exact release and MU1440')


def validate_media(directory):
    """Validate an explicitly prepared media directory without corpus pins."""
    directory = Path(directory).resolve(strict=True)
    manifest_path = directory / 'ui-manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if (manifest.get('firmware_profile') != ID or
            manifest.get('firmware_train') != RELEASE or
            str(manifest.get('firmware_mu')) != MU or
            manifest.get('emulator_only') is not True):
        raise ValueError('Media is not explicitly marked for this experimental profile')
    metadata_path = manifest.get('metadata')
    if not isinstance(metadata_path, str) or not metadata_path or Path(metadata_path).is_absolute():
        raise ValueError('Experimental media must name a relative metadata file')
    metadata_file = (directory / metadata_path).resolve(strict=True)
    if not metadata_file.is_relative_to(directory) or not metadata_file.is_file():
        raise ValueError('Media metadata path escapes the media directory')
    read_metadata(metadata_file)
    for name in ('nor.bin', 'iram.bin', 'emmc.raw'):
        path = directory / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f'Missing or empty prepared media: {name}')
    return manifest


def firmware_identity():
    """Return only the known train/MU strings; no VIN or vehicle coding."""
    return {
        '30:1966083': {'type': 'string', 'value': MU},
        '30:1966084': {'type': 'string', 'value': RELEASE},
        '46924065:401': {
            'type': 'blob', 'value': RELEASE.encode().ljust(32, b' ').hex()
        },
    }

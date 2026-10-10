"""Viewer profiles selected from the update's [common] release and variants.

The default remains Volkswagen for existing callers without metadata. Unknown
metadata uses a generic title rather than misidentifying another VAG brand.
"""
import os
import re
from pathlib import Path
from skoda_profile import ID as SKODA_ID, RELEASE as SKODA_RELEASE, validate_metadata, validate_media, read_metadata


def common_metadata(path=None):
    path = path or (os.environ.get('MHI2_FIRMWARE_META') or os.environ.get('MHI2_METADATA'))
    if not path:
        return {}
    result = {}
    common = False
    for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
        line = line.strip()
        if line.startswith('['):
            common = line.lower() == '[common]'
        elif common:
            match = re.fullmatch(r'(\w+)\s*=\s*"(.*)"', line)
            if match:
                result[match[1]] = match[2]
    return result


PROFILES = {
    SKODA_ID: dict(brand='skoda', title='Škoda Columbus MHI2 (SKG13 P4526)',
                   keyboard=13, touch=True, train=SKODA_RELEASE, mu='1440',
                   oscillator_12mhz=None, metadata='',
                   main_viewport=(1280, 640), touch_wire_divisor=(2, 2), cockpit_viewport=(1280, 480)),
    'porsche': dict(brand='porsche', title='Porsche PCM 4.0', keyboard=13, touch=True,
                    train='MHI2_ER_POG11_K5126', mu='1394', oscillator_12mhz=False,
                    metadata=''),
    'vw': dict(brand='volkswagen', title='Volkswagen MHI2', keyboard=13, touch=True,
               train='MHI2_ER_VWG11_K3342', mu='1427', oscillator_12mhz=False,
               metadata='/home/iscle/Downloads/mhi2-analysis/extracted/metainfo2.txt'),
    'audi-a3': dict(brand='audi', title='Audi A3 MMI', keyboard=1, touch=False,
                    train='MHI2_ER_AU37x_P5089', mu='1326', oscillator_12mhz=True,
                    metadata='/home/iscle/mhi2-audi-port/firmware/metainfo2.txt'),
}


def select_profile(metadata=None, brand='auto'):
    if brand not in ('auto', 'porsche', 'volkswagen', 'audi', 'skoda'):
        raise ValueError('Unknown UI brand: ' + brand)
    metadata = common_metadata() if metadata is None else metadata
    release = metadata.get('release', '')
    if brand == 'skoda' or release == SKODA_RELEASE or '_SKG' in release:
        validate_metadata(metadata)
        if brand not in ('auto', 'skoda'):
            raise ValueError('Skoda metadata conflicts with selected UI brand')
        brand = 'skoda'
    if brand == 'auto':
        variants = ' '.join(v for k, v in metadata.items() if k.startswith('variant'))
        if re.search(r'_(?:PO|PAG)\w*_', release) or '-PO-' in variants:
            brand = 'porsche'
        elif re.search(r'_AU37\w*_', release):
            brand = 'audi'
        elif not metadata or re.search(r'_VW\w*_', release) or '-VW-' in variants:
            brand = 'volkswagen'
        else:
            brand = 'generic'
    name = {'porsche': 'porsche', 'volkswagen': 'vw', 'audi': 'audi-a3', 'skoda': SKODA_ID}.get(brand)
    profile = dict(PROFILES[name]) if name else dict(
        title='MHI2', keyboard=13, touch=True, train='', mu='',
        oscillator_12mhz=False, metadata='')
    profile.update(name=name, brand=brand, release=release)
    if release:
        profile['train'] = release
    if metadata.get('MUVersion'):
        profile['mu'] = metadata['MUVersion']
    return profile


def current():
    name = os.environ.get('MHI2_FIRMWARE')
    if name is not None and name not in PROFILES:
        raise ValueError('Unknown MHI2 firmware: ' + name)
    metadata = common_metadata()
    brand = PROFILES[name]['brand'] if name else 'auto'
    if name == SKODA_ID and not metadata:
        raise ValueError('Skoda metadata required; no implicit VW fallback')
    if (name == SKODA_ID or metadata.get('release') == SKODA_RELEASE) and (os.environ.get('MHI2_FIRMWARE_META') or os.environ.get('MHI2_METADATA')):
        metadata = read_metadata(os.environ.get('MHI2_FIRMWARE_META') or os.environ['MHI2_METADATA'])
    profile = select_profile(metadata, brand)
    path = os.environ.get('MHI2_FIRMWARE_META') or os.environ.get('MHI2_METADATA')
    if path:
        profile['metadata'] = path
    return profile


def configure(name, media=None):
    """Select matching media, input wiring and clock before spawning helpers."""
    from pathlib import Path
    if name not in PROFILES:
        raise ValueError('Unknown MHI2 firmware: ' + name)
    profile = dict(PROFILES[name])
    if name == SKODA_ID:
        if not media:
            raise ValueError('Skoda requires independently prepared --media')
        validate_media(media)
        oscillator = os.environ.get('MHI2_SKODA_OSCILLATOR_12MHZ')
        if oscillator not in ('0', '1') or os.environ.get('MHI2_SKODA_INPUT_EXPERIMENT') != '1':
            raise ValueError('Skoda clock/input unverified; explicit experiment overrides required')
        os.environ['MHI2_OSCILLATOR_12MHZ'] = oscillator
    else:
        os.environ['MHI2_OSCILLATOR_12MHZ'] = str(int(profile['oscillator_12mhz']))
    os.environ['MHI2_FIRMWARE'] = name
    if name == 'porsche' and not media:
        raise ValueError('Porsche requires --media with prepared Porsche images')
    if name == 'audi-a3' or media:
        directory = Path(media or '/home/iscle/mhi2-audi-port/media/audi-a3').resolve()
        import json
        manifest = json.loads((directory / 'ui-manifest.json').read_text())
        if manifest.get('firmware_train') != profile['train']:
            raise ValueError('Media firmware does not match selected profile')
        if manifest.get('metadata'):
            metadata = directory / manifest['metadata']
            if not metadata.is_file():
                raise ValueError('Missing firmware metadata: ' + str(metadata))
            os.environ['MHI2_METADATA'] = str(metadata)
            os.environ['MHI2_FIRMWARE_META'] = str(metadata)
        if manifest.get('shader_cache'):
            cache = directory / manifest['shader_cache']
            if not (cache / 'manifest.json').is_file():
                raise ValueError('Missing firmware shader cache: ' + str(cache))
            os.environ['MHI2_SHADER_CACHE'] = str(cache)
        os.environ.update(MHI2_DEBUG_NOR=str(directory / 'nor.bin'),
                          MHI2_IRAM=str(directory / 'iram.bin'),
                          MHI2_EMMC=str(directory / 'emmc.raw'),
                          MHI2_MEDIA_ROOT=str(directory.parent))
    else:
        root = Path('/home/iscle/Downloads/mhi2-analysis/qemu')
        os.environ.setdefault('MHI2_DEBUG_NOR', str(root / 'k3342-gl-debug-nor.bin'))
        disk = root / 'emmc-gl.raw'
        for name, marker in [('emmc-maps', 'maps'), ('emmc-complete', 'speech')]:
            candidate = root / (name + '.raw')
            if candidate.is_file() and (root / (name + '.' + marker + '.json')).is_file():
                disk = candidate
        os.environ.setdefault('MHI2_EMMC', str(disk))
    return current()

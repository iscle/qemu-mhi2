"""Firmware-specific media and production input identities."""
import os

PROFILES = {
    'vw': dict(title='Volkswagen MHI2', keyboard=13, touch=True,
               train='MHI2_ER_VWG11_K3342', mu='1427', oscillator_12mhz=False,
               metadata='/home/iscle/Downloads/mhi2-analysis/extracted/metainfo2.txt'),
    'audi-a3': dict(title='Audi A3 MMI', keyboard=1, touch=False,
                    train='MHI2_ER_AU37x_P5089', mu='1326', oscillator_12mhz=True,
                    metadata='/home/iscle/mhi2-audi-port/firmware/metainfo2.txt'),
}


def current():
    name = os.environ.get('MHI2_FIRMWARE', 'vw')
    if name not in PROFILES:
        raise ValueError('Unknown MHI2 firmware: ' + name)
    return PROFILES[name]


def configure(name, media=None):
    """Select matching media, input wiring and clock before spawning helpers."""
    from pathlib import Path
    os.environ['MHI2_FIRMWARE'] = name
    profile = current()
    os.environ['MHI2_OSCILLATOR_12MHZ'] = str(int(profile['oscillator_12mhz']))
    if name == 'audi-a3' or media:
        directory = Path(media or '/home/iscle/mhi2-audi-port/media/audi-a3')
        import json
        manifest = json.loads((directory / 'ui-manifest.json').read_text())
        if manifest.get('firmware_train') != profile['train']:
            raise ValueError('Media firmware does not match selected profile')
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
    return profile

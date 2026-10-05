"""Viewer profiles selected from the update's [common] release and variants.

The default remains Volkswagen for existing callers without metadata. Unknown
metadata uses a generic title rather than misidentifying another VAG brand.
"""
import os
import re
from pathlib import Path


def common_metadata(path=None):
    path = path or os.environ.get('MHI2_FIRMWARE_META')
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


def select_profile(metadata=None, brand='auto'):
    if brand not in ('auto', 'porsche', 'volkswagen'):
        raise ValueError('Unknown UI brand: ' + brand)
    metadata = common_metadata() if metadata is None else metadata
    release = metadata.get('release', '')
    if brand == 'auto':
        variants = ' '.join(v for k, v in metadata.items() if k.startswith('variant'))
        if re.search(r'_(?:PO|PAG)\w*_', release) or '-PO-' in variants:
            brand = 'porsche'
        elif not metadata or re.search(r'_VW\w*_', release) or '-VW-' in variants:
            brand = 'volkswagen'
        else:
            brand = 'generic'
    return {'brand': brand, 'release': release,
            'title': {'porsche': 'Porsche PCM 4.0', 'volkswagen': 'Volkswagen MHI2',
                      'generic': 'MHI2'}[brand]}

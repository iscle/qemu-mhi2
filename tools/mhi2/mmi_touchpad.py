"""Local MMI touch device: stroke matching and firmware DSI character replies.

Recognition is a small geometric prototype for digits and block Latin/Cyrillic letters,
not the physical controller's proprietary recognizer or a calibrated model.
"""
import json
import math
import time
from pathlib import Path

STATE_PATH = Path('/tmp/mhi2-touchpad-state.json')
CURSOR_PATH = Path('/tmp/mhi2-map-cursor.json')
CHARACTER_MODES = frozenset((12, 19, 20, 21, 22, 23, 26))
MAP_MODES = frozenset((25,))
# Separate polylines are pen lifts. Shape matching ignores stroke order/direction.
TEMPLATES = {
    '0': [[(0.5,0),(0.1,0.15),(0,0.5),(0.1,0.85),(0.5,1),(0.9,0.85),(1,0.5),(0.9,0.15),(0.5,0)]],
    '1': [[(0.25,0.2),(0.5,0),(0.5,1)],[(0.2,1),(0.8,1)]],
    '2': [[(0,0.2),(0.2,0),(0.8,0),(1,0.2),(0.9,0.4),(0,1),(1,1)]],
    '3': [[(0,0),(0.8,0),(1,0.2),(0.7,0.5),(0.3,0.5),(0.8,0.5),(1,0.8),(0.8,1),(0,1)]],
    '4': [[(0.8,1),(0.8,0)],[(0.8,0),(0,0.7),(1,0.7)]],
    '5': [[(1,0),(0,0),(0,0.5),(0.7,0.5),(1,0.7),(0.9,0.9),(0.5,1),(0,0.9)]],
    '6': [[(0.9,0.1),(0.6,0),(0.2,0.3),(0,0.7),(0.2,1),(0.7,1),(1,0.8),(0.9,0.6),(0.5,0.5),(0,0.7)]],
    '7': [[(0,0),(1,0),(0.3,1)]],
    '8': [[(0.5,0),(0.1,0.1),(0,0.3),(0.5,0.5),(1,0.7),(0.9,0.9),(0.5,1),(0.1,0.9),(0,0.7),(0.5,0.5),(1,0.3),(0.9,0.1),(0.5,0)]],
    '9': [[(1,0.3),(0.8,0),(0.2,0),(0,0.2),(0.1,0.5),(0.6,0.6),(1,0.3),(0.8,0.8),(0.4,1)]],
    'A': [[(0,1),(0.5,0),(1,1)],[(0.2,0.6),(0.8,0.6)]],
    'B': [[(0,1),(0,0),(0.7,0),(1,0.2),(0.7,0.5),(0,0.5),(0.7,0.5),(1,0.8),(0.7,1),(0,1)]],
    'C': [[(1,0.1),(0.5,0),(0.1,0.2),(0,0.5),(0.1,0.8),(0.5,1),(1,0.9)]],
    'D': [[(0,1),(0,0),(0.6,0),(1,0.3),(1,0.7),(0.6,1),(0,1)]],
    'E': [[(1,0),(0,0),(0,1),(1,1)],[(0,0.5),(0.8,0.5)]],
    'F': [[(1,0),(0,0),(0,1)],[(0,0.5),(0.8,0.5)]],
    'G': [[(1,0.1),(0.5,0),(0.1,0.2),(0,0.5),(0.1,0.8),(0.5,1),(1,0.9),(1,0.5),(0.6,0.5)]],
    'H': [[(0,0),(0,1)],[(1,0),(1,1)],[(0,0.5),(1,0.5)]],
    'I': [[(0,0),(1,0)],[(0.5,0),(0.5,1)],[(0,1),(1,1)]],
    'J': [[(0,0),(1,0),(1,0.8),(0.8,1),(0.2,1),(0,0.8)]],
    'K': [[(0,0),(0,1)],[(1,0),(0,0.5),(1,1)]],
    'L': [[(0,0),(0,1),(1,1)]],
    'M': [[(0,1),(0,0),(0.5,0.6),(1,0),(1,1)]],
    'N': [[(0,1),(0,0),(1,1),(1,0)]],
    'O': [[(0.5,0),(0,0.2),(0,0.8),(0.5,1),(1,0.8),(1,0.2),(0.5,0)]],
    'P': [[(0,1),(0,0),(0.8,0),(1,0.2),(0.8,0.5),(0,0.5)]],
    'Q': [[(0.5,0),(0,0.2),(0,0.8),(0.5,1),(1,0.8),(1,0.2),(0.5,0)],[(0.6,0.7),(1.1,1.1)]],
    'R': [[(0,1),(0,0),(0.8,0),(1,0.2),(0.8,0.5),(0,0.5)],[(0.5,0.5),(1,1)]],
    'S': [[(1,0.1),(0.5,0),(0,0.2),(0.1,0.4),(0.9,0.6),(1,0.8),(0.5,1),(0,0.9)]],
    'T': [[(0,0),(1,0)],[(0.5,0),(0.5,1)]],
    'U': [[(0,0),(0,0.8),(0.3,1),(0.7,1),(1,0.8),(1,0)]],
    'V': [[(0,0),(0.5,1),(1,0)]],
    'W': [[(0,0),(0.25,1),(0.5,0.4),(0.75,1),(1,0)]],
    'X': [[(0,0),(1,1)],[(1,0),(0,1)]],
    'Y': [[(0,0),(0.5,0.5),(1,0)],[(0.5,0.5),(0.5,1)]],
    'Z': [[(0,0),(1,0),(0,1),(1,1)]],
}

CYRILLIC = 'АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЬЮЯ'
for cyrillic, latin in zip('АВЕКМНОРСТХ', 'ABEKMHOPCTX'):
    TEMPLATES[cyrillic] = TEMPLATES[latin]
TEMPLATES.update({
    'Б': [[(1,0),(0,0),(0,1),(0.7,1),(1,0.8),(0.7,0.5),(0,0.5)]],
    'Г': [[(0,1),(0,0),(1,0)]],
    'Д': [[(0,1),(0,0.8),(1,0.8),(1,1)],[(0.15,0.8),(0.35,0),(0.8,0),(0.8,0.8)]],
    'Ж': [[(0,0),(1,1)],[(0.5,0),(0.5,1)],[(1,0),(0,1)]],
    'З': TEMPLATES['3'],
    'И': [[(0,0),(0,1),(1,0),(1,1)]],
    'Й': [[(0,0.25),(0,1),(1,0.25),(1,1)],[(0.3,0),(0.5,0.1),(0.7,0)]],
    'Л': [[(0,1),(0.35,0),(1,0),(1,1)]],
    'П': [[(0,1),(0,0),(1,0),(1,1)]],
    'У': [[(0,0),(0.5,0.55)],[(1,0),(0.1,1)]],
    'Ф': [[(0.5,0),(0.5,1)],[(0.5,0.15),(0,0.3),(0,0.7),(0.5,0.85),(1,0.7),(1,0.3),(0.5,0.15)]],
    'Ц': [[(0,0),(0,0.85),(0.85,0.85),(0.85,0)],[(0.85,0.85),(1,0.85),(1,1)]],
    'Ч': [[(0,0),(0,0.5),(1,0.5)],[(1,0),(1,1)]],
    'Ш': [[(0,0),(0,1),(1,1),(1,0)],[(0.5,0),(0.5,1)]],
    'Щ': [[(0,0),(0,0.85),(0.85,0.85),(0.85,0)],[(0.425,0),(0.425,0.85)],[(0.85,0.85),(1,0.85),(1,1)]],
    'Ь': [[(0,0),(0,1),(0.7,1),(1,0.8),(0.7,0.5),(0,0.5)]],
    'Ю': [[(0,0),(0,1)],[(0,0.5),(0.35,0.5)],[(0.65,0),(0.35,0.2),(0.35,0.8),(0.65,1),(1,0.8),(1,0.2),(0.65,0)]],
    'Я': [[(1,1),(1,0),(0.2,0),(0,0.25),(0.2,0.5),(1,0.5)],[(0.6,0.5),(0,1)]],
})


def cloud(strokes):
    points = [p for stroke in strokes for p in stroke]
    if len(points) < 2:
        return []
    minx, miny = min(p[0] for p in points), min(p[1] for p in points)
    width = max(p[0] for p in points) - minx
    height = max(p[1] for p in points) - miny
    scale = max(width, height)
    if scale <= 0:
        return []
    segments = [(a, b, math.dist(a, b)) for stroke in strokes for a, b in zip(stroke, stroke[1:])]
    length = sum(d for _, _, d in segments)
    if length <= 0:
        return []
    result = []
    # Uniform arc-length sampling; pen-up travel is never part of the glyph.
    for i in range(64):
        target = length * i / 63
        for a, b, distance in segments:
            if distance and target <= distance:
                f = target / distance
                result.append(((a[0] + f*(b[0]-a[0])-minx-width/2)/scale,
                               (a[1] + f*(b[1]-a[1])-miny-height/2)/scale))
                break
            target -= distance
        else:
            b = segments[-1][1]
            result.append(((b[0]-minx-width/2)/scale, (b[1]-miny-height/2)/scale))
    for stroke in strokes:
        if stroke and sum(math.dist(a, b) for a, b in zip(stroke, stroke[1:])) < scale * 0.02:
            x, y = stroke[0]
            result.extend([((x-minx-width/2)/scale, (y-miny-height/2)/scale)] * 4)
    return result


CLOUDS = {char: cloud(strokes) for char, strokes in TEMPLATES.items()}


def recognize(strokes, digits_only=False, alphabet='latin'):
    """Return candidate shapes, not probability/confidence claims."""
    points = cloud(strokes)
    if not points:
        return []
    def distance(other):
        def nearest(a, b):
            return sum(min(math.dist(p, q) for q in b) for p in a)/len(a)
        return (nearest(points, other) + nearest(other, points))/2
    allowed = '0123456789' + (CYRILLIC if alphabet == 'cyrillic' else 'ABCDEFGHIJKLMNOPQRSTUVWXYZ')
    ranked = sorted((distance(other), char) for char, other in CLOUDS.items()
                    if char in allowed and (not digits_only or char.isdigit()))
    # Reject unrelated shapes rather than forcing a nearest symbol.
    return [(char, round(max(0, 1-score/0.3), 3)) for score, char in ranked[:3] if score < 0.13]


class TouchDevice:
    """RCC companion device; actual handwriting recognition lives on the host."""
    def __init__(self, enabled=False):
        self.enabled = enabled
        self.mode = 0
        self.language = ''
        self.language_code = 0
        self.generation = time.time_ns()
        self.last_publish = 0
        self.last_character = ''
        self.last_error = ''

    def publish(self, ready):
        now = time.time()
        if now-self.last_publish < 0.5:
            return
        self.last_publish = now
        state = dict(enabled=self.enabled, ready=ready, mode=self.mode,
                     language=self.language, generation=self.generation,
                     updated=now, last_character=self.last_character, error=self.last_error)
        temporary = STATE_PATH.with_suffix('.tmp')
        try:
            temporary.write_text(json.dumps(state))
            temporary.replace(STATE_PATH)
        except OSError:
            pass

    def request(self, services, method, data):
        """Handle only the touch-related calls; retain all other service handling."""
        import struct
        from rcc_services import decode
        from rcc_persistence import Reader
        if not self.enabled:
            return None
        reply = lambda name, values: services.reply('DSIKeyPanel', name, values)
        if method == 15:
            panel, mode = struct.unpack('!ii', data)
            if panel != 9:
                return None
            self.mode = mode
            self.last_error = ''
            self.generation += 1
            print('MMI touch mode', self.mode, flush=True)
            return [reply('updateRecognizerMode', [9, self.mode, 1])]
        if method in (14, 28):
            reader = Reader(data)
            panel = decode('Int32', reader)
            language = decode('OptionalString', reader)
            code = decode('Int32', reader) if method == 28 else 0
            if reader.pos != len(data) or panel != 9:
                return None
            if (self.language, self.language_code) != (language or '', code):
                self.generation += 1
            self.language, self.language_code = language or '', code
            return [reply('updateRecognizerLanguage2', [9, self.language, code, 1])]
        if method == 40:
            if struct.unpack('!i', data)[0] != 9:
                return None
            self.generation += 1
            return []
        if method == 37:
            panel, x, y, w, h = struct.unpack('!iiiii', data)
            if panel != 9:
                return None
            return [reply('updateTouchSensitiveArea', [panel, x, y, w, h, 1])]
        return None

    def event(self, services, event):
        if not self.enabled:
            raise ValueError('MMI touch is only enabled for the local Audi A4 profile')
        map_input = event.get('input_mode') == 'map'
        if self.mode not in (MAP_MODES if map_input else CHARACTER_MODES):
            raise ValueError('Open the navigation map first' if map_input else 'Open an MMI handwriting input field first')
        if int(event.get('generation', -1)) != self.generation:
            raise ValueError('MMI input field changed; draw the symbol again')
        action = event['action']
        if action == 'character':
            if map_input:
                raise ValueError('Map mode does not accept handwriting characters')
            character = event.get('character', '')
            if len(character) != 1 or character not in TEMPLATES:
                raise ValueError('Unsupported handwriting character')
            # A confirmed candidate is a single result, not invented RCC confidence.
            self.last_character = character
            self.last_error = ''
            return services.reply('DSIKeyPanel', 'updateCharacterEvent2', [9, [character], [100], 1])
        if action not in ('press', 'move', 'release'):
            raise ValueError('Unknown MMI touch action')
        x, y = int(event['x']), int(event['y'])
        if not 0 <= x <= 1000 or not 0 <= y <= 1000:
            raise ValueError('MMI touch coordinates outside pad')
        return services.reply('DSIKeyPanel', 'updateGesture2',
                              [9, 0, int(action != 'release'), False, x, y, 0, 0,
                               int(time.monotonic()*1000) & 0x7fffffff, 1])

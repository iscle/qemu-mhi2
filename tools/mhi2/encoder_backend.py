"""Probe host H.264 encoders; pass the working codec to asynchronous workers."""
import os
from pathlib import Path
import subprocess
import sys


def encoder_options(codec, device=None):
    """Options paired with host/encoder.h, also used for a real one-frame probe."""
    hardware = codec == 'h264_vaapi'
    args = ['-vaapi_device', device] if hardware else []
    args += ['-f', 'lavfi', '-i', 'color=black:s=800x480:r=5',
             '-vf', 'format=nv12,hwupload' if hardware else 'format=yuv420p',
             '-c:v', codec, '-threads', '1', '-profile:v',
             'constrained_baseline' if codec in ('h264_vaapi', 'libopenh264') else 'baseline',
             '-bf', '0', '-b:v', '2000000']
    if hardware:
        args += ['-async_depth', '1']
    elif codec == 'h264_videotoolbox':
        args += ['-allow_sw', '0', '-realtime', '1']
    elif codec == 'libx264':
        args += ['-preset', 'ultrafast', '-tune', 'zerolatency']
    else:
        args += ['-rc_mode', 'bitrate']
    return args


def configure_encoder(env=None):
    env = os.environ if env is None else env
    mode = env.get('MHI2_ENCODER', 'auto')
    if mode not in ('auto', 'hardware', 'vaapi', 'videotoolbox', 'software'):
        raise ValueError('MHI2_ENCODER must be auto, hardware, vaapi, videotoolbox, or software')
    explicit = env.get('MHI2_VAAPI_DEVICE')
    env.pop('MHI2_ENCODER_CODEC', None)
    candidates = []
    if mode != 'software':
        if mode == 'videotoolbox' or (sys.platform == 'darwin' and mode != 'vaapi' and not explicit):
            candidates.append(('h264_videotoolbox', None))
        else:
            devices = [explicit] if explicit else sorted(str(p) for p in Path('/dev/dri').glob('renderD*'))
            candidates += [('h264_vaapi', device) for device in devices]
    required = mode in ('hardware', 'vaapi', 'videotoolbox') or (explicit and mode != 'software')
    if not required:
        candidates += [('libx264', None), ('libopenh264', None)]
    errors = []
    for codec, device in candidates:
        command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-filter_threads', '1']
        command += encoder_options(codec, device) + ['-frames:v', '1', '-f', 'null', '-']
        try:
            result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.PIPE, timeout=8, env=dict(env))
            if result.returncode == 0:
                env['MHI2_ENCODER_CODEC'] = codec
                if device:
                    env['MHI2_VAAPI_DEVICE'] = device
                else:
                    env.pop('MHI2_VAAPI_DEVICE', None)
                kind = 'hardware H.264' if codec.startswith('h264_') else 'software H.264'
                Path('/tmp/mhi2-encoder-probe.log').write_text('\n'.join(errors)+f'\nSelected {codec}\n')
                return f'Cockpit encoder: {kind} {codec}' + (f' on {device}' if device else '') + ' (asynchronous).'
            errors.append(f'{codec} {device or ""}: {result.stderr.decode(errors="replace").strip()}')
        except (OSError, subprocess.TimeoutExpired) as exc:
            errors.append(f'{codec}: {exc}')
    reason = '\n'.join(errors) or 'No hardware encoder device is accessible.'
    Path('/tmp/mhi2-encoder-probe.log').write_text(reason+'\n')
    env.pop('MHI2_VAAPI_DEVICE', None)
    raise RuntimeError(('Requested hardware encoder unavailable: ' if required else 'No usable H.264 encoder: ')+reason)

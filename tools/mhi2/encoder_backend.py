"""Select a working H.264 hardware encoder before starting the emulator."""
import os
from pathlib import Path
import subprocess


def configure_encoder(env=None):
    env = os.environ if env is None else env
    mode = env.get('MHI2_ENCODER', 'auto')
    if mode not in ('auto', 'vaapi', 'software'):
        raise ValueError('MHI2_ENCODER must be auto, vaapi, or software')
    explicit = env.get('MHI2_VAAPI_DEVICE')
    if mode == 'software':
        env.pop('MHI2_VAAPI_DEVICE', None)
        return 'Cockpit encoder: asynchronous software (explicit diagnostic selection).'
    devices = [explicit] if explicit else sorted(str(p) for p in Path('/dev/dri').glob('renderD*'))
    errors = []
    for device in devices:
        command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-filter_threads', '1', '-vaapi_device', device,
                   '-f', 'lavfi', '-i', 'color=black:s=800x480:r=5',
                   '-vf', 'format=nv12,hwupload', '-c:v', 'h264_vaapi',
                   '-threads', '1', '-profile:v', 'constrained_baseline', '-bf', '0', '-async_depth', '1',
                   '-b:v', '2000000', '-frames:v', '1', '-f', 'null', '-']
        try:
            result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.PIPE, timeout=8, env=dict(env))
            if result.returncode == 0:
                env['MHI2_VAAPI_DEVICE'] = device
                return f'Cockpit encoder: hardware H.264 VA-API on {device} (asynchronous).'
            errors.append(f'{device}: {result.stderr.decode(errors="replace").strip()}')
        except (OSError, subprocess.TimeoutExpired) as exc:
            errors.append(f'{device}: {exc}')
    reason = '\n'.join(errors) or 'No /dev/dri/renderD* device is accessible.'
    Path('/tmp/mhi2-encoder-probe.log').write_text(reason+'\n')
    if mode == 'vaapi' or explicit:
        raise RuntimeError('Requested hardware encoder unavailable: '+reason)
    env.pop('MHI2_VAAPI_DEVICE', None)
    return ('Cockpit encoder: hardware unavailable; using asynchronous software. '
            'See /tmp/mhi2-encoder-probe.log.')

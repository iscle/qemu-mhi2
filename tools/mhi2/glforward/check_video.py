#!/usr/bin/env python3
"""Same real H.264 pixel/ownership tests on Linux and macOS, no firmware needed."""
import os
import re
import selectors
import struct
from pathlib import Path
import subprocess
import tempfile
import sys

scripts = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(scripts))
from encoder_backend import configure_encoder

with tempfile.TemporaryDirectory(prefix='mhi2-video-') as directory:
    root = Path(directory)
    env = dict(os.environ, MHI2_ENCODER='software')
    env.pop('MHI2_VAAPI_DEVICE', None)
    configure_encoder(env)
    fixture = root/'red.h264'
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                    '-i', 'color=red:s=800x480:r=5', '-c:v', env['MHI2_ENCODER_CODEC'],
                    '-bf', '0', '-frames:v', '5', '-bsf:v', 'h264_metadata=aud=insert',
                    '-f', 'h264', str(fixture)], check=True)
    exe = root/'decoder'
    subprocess.run(['bash', str(scripts/'glforward/build_host.sh'), str(exe),
                    str(scripts/'glforward/host/check_video_decoder.c')], check=True)
    # Defaults to software for reproducible CI; set MHI2_DECODER=hardware for
    # a strict hardware test on a machine with a supported driver and GPU.
    env['MHI2_DECODER'] = os.environ.get('MHI2_DECODER', 'software')
    for framing in ([], ['avcc']):
        subprocess.run([str(exe), str(fixture), *framing], env=env, check=True, timeout=20)

    # Exercise the actual wire protocol and both published display planes.
    bridge = root/'bridge'
    subprocess.run(['bash', str(scripts/'glforward/build_host.sh'), str(bridge)], check=True)
    main, cluster = root/'main.ppm', root/'cluster.ppm'
    env.update(MHI2_GL_FRAME=str(main), MHI2_CLUSTER_VIDEO_FRAME=str(cluster))
    data = fixture.read_bytes()
    starts = list(re.finditer(rb'\x00\x00\x00?\x01', data))
    units, current = [], bytearray()
    for index, match in enumerate(starts):
        end = starts[index+1].start() if index+1 < len(starts) else len(data)
        nal = data[match.end():end]
        if nal[0] & 31 == 9 and current:
            units.append(bytes(current)); current.clear()
        current += b'\0\0\0\1' + nal
    if current:
        units.append(bytes(current))
    with (root/'bridge.log').open('wb') as log:
        process = subprocess.Popen([str(bridge)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=log, env=env)
        try:
            def words(*values):
                return struct.pack('<'+'I'*len(values), *values)
            def send(op, payload, reply=False):
                process.stdin.write(words(op, len(payload))+payload)
                process.stdin.flush()
                if reply:
                    with selectors.DefaultSelector() as ready:
                        ready.register(process.stdout, selectors.EVENT_READ)
                        assert ready.select(10), 'Bridge response timeout'
                    assert process.stdout.read(4) == words(0)
            send(1, words(800, 480))
            send(119, words(20))
            send(134, words(1, 800, 480, 1), True)
            background = main.read_bytes()
            for unit in units:
                send(135, words(1, len(unit))+unit, True)
            pixels = cluster.read_bytes().split(b'\n', 3)[3]
            assert pixels[0] > 240 and pixels[1] < 12 and pixels[2] < 12
            assert main.read_bytes() == background, 'Cluster video contaminated the center screen'
            attrs = bytearray(68)
            struct.pack_into('<I', attrs, 0, 1)
            struct.pack_into('<8i', attrs, 16, 10, 20, 100, 80, 0, 0, 100, 80)
            send(136, attrs, True)
            assert cluster.read_bytes().startswith(b'P6\n100 80\n255\n')
            attrs[65] = 1
            send(136, attrs, True)
            assert not cluster.exists(), 'Hidden video retained a stale cluster frame'
            send(137, words(1), True)
            send(134, words(1, 800, 480, 0), True)
            for unit in units:
                send(135, words(1, len(unit))+unit, True)
            pixels = main.read_bytes().split(b'\n', 3)[3]
            assert pixels[0] > 240 and pixels[1] < 12 and pixels[2] < 12
            send(137, words(1), True)
            assert main.read_bytes() == background, 'Close failed to restore the GLES frame'
            process.stdin.close()
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill(); process.wait()
    print('PASS: native H.264 wire protocol, center/cluster pixels, crop, hide and restoration')

    if sys.platform != 'darwin':
        env['MHI2_DECODER'] = 'hardware'
        env['MHI2_DECODE_DEVICE'] = str(root/'no-render-device')
        subprocess.run([str(exe), str(fixture), 'unavailable'], env=env, check=True, timeout=20)

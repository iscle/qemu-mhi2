#!/usr/bin/env python3
"""Real codec round-trip and stalled-codec isolation; no GPU required."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from encoder_backend import configure_encoder
from most_sink import TransportStream


class Checks(unittest.TestCase):
    def test_hardware_probe_selection_and_explicit_failure(self):
        env = dict(os.environ, MHI2_VAAPI_DEVICE='/dev/dri/renderD129', MHI2_ENCODER='vaapi')
        with patch('encoder_backend.subprocess.run') as run, patch('encoder_backend.Path.write_text'):
            run.return_value.returncode = 0
            self.assertIn('hardware H.264', configure_encoder(env))
            args = run.call_args.args[0]
            self.assertIn('h264_vaapi', args)
            self.assertIn('format=nv12,hwupload', args)
            run.return_value.returncode = 1
            run.return_value.stderr = b'Unsupported encoding profile'
            with self.assertRaises(RuntimeError): configure_encoder(env)

    def test_auto_probes_next_device_and_software_skips_probe(self):
        env = dict(os.environ)
        env.pop('MHI2_VAAPI_DEVICE', None)
        env['MHI2_ENCODER'] = 'auto'
        with patch('encoder_backend.Path.glob', return_value=[Path('/dev/dri/renderD129'), Path('/dev/dri/renderD128')]), patch('encoder_backend.subprocess.run') as run:
            run.side_effect = [subprocess.CompletedProcess([], 1, stderr=b'unsupported'),
                               subprocess.CompletedProcess([], 0, stderr=b'')]
            self.assertIn('/dev/dri/renderD129', configure_encoder(env))
            self.assertEqual(env['MHI2_VAAPI_DEVICE'], '/dev/dri/renderD129')
            self.assertEqual(run.call_count, 2)
            env['MHI2_ENCODER'] = 'software'
            run.reset_mock()
            self.assertIn('software', configure_encoder(env))
            self.assertNotIn('MHI2_VAAPI_DEVICE', env)
            run.assert_not_called()

    def test_guest_cadence_and_read_bounds(self):
        source = Path(__file__).resolve().parent/'glforward/tests/encoder_cadence.c'
        with tempfile.TemporaryDirectory(prefix='mhi2-cadence-check-') as tmp:
            exe = str(Path(tmp)/'cadence')
            subprocess.run(['cc', '-O2', str(source), '-o', exe], check=True)
            subprocess.run([exe], check=True, timeout=5)

    def test_real_stream_and_stalled_worker(self):
        root = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix='mhi2-encoder-check-') as tmp:
            tmp = Path(tmp);exe = tmp/'encoder-test'
            subprocess.run(['cc', '-O2', '-pthread', str(root/'glforward/tests/encoder_async.c'), '-o', str(exe)], check=True)
            env = dict(os.environ);env.pop('MHI2_VAAPI_DEVICE', None)
            stream = tmp/'video.ts'
            subprocess.run([str(exe), 'real', str(stream)], env=env, check=True, timeout=15)
            transport = TransportStream();self.assertEqual(transport.extract(stream.read_bytes()), stream.read_bytes())
            self.assertEqual(transport.discontinuities, 0)
            result = subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-i',str(stream),
                                     '-f','rawvideo','-pix_fmt','rgb24','pipe:1'], capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, b'')
            self.assertGreater(len(result.stdout), 800*480*3*5)
            fake = tmp/'ffmpeg';fake.write_text('#!/bin/sh\nexec sleep 10\n');fake.chmod(0o755)
            env['PATH'] = str(tmp)+os.pathsep+env['PATH']
            subprocess.run([str(exe),'stalled',str(tmp/'stalled.ts')],env=env,check=True,timeout=5)


if __name__ == '__main__': unittest.main()

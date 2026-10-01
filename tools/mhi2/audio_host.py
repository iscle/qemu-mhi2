"""PCM endpoint for the original K3342 speech queues (48 kHz mono S16LE).

UDP protocol is local to this emulation, not the production RCC wire protocol.
Microphone capture starts only while the native microphone queue is active.
"""
import os
import struct
import subprocess
import time
from pathlib import Path


class AudioEndpoint:
    def __init__(self):
        self.player = None
        self.recorder = None
        self.last_mic = 0
        self.pending = bytearray()
        self.retry = {"player": 0, "recorder": 0}
        self.playback_blocks = 0
        self.microphone_blocks = 0
        self.log = None

    def process(self, packet):
        if len(packet) < 12 or packet[:4] != b'AUD0':
            return None
        kind, stream = packet[4:6]
        if kind == 1 and 10 <= stream < 19 and len(packet) == 1036:
            self.playback_blocks += 1
            if self.log is None:
                self.log = open('/tmp/mhi2-speech-output.s16le', 'wb')
            self.log.write(packet[12:])
            self.log.flush()
            if self.player is None and time.monotonic() >= self.retry["player"]:
                self.player = self.start('pw-play', playback=True)
            if self.player and self.player.poll() is None:
                try:
                    os.write(self.player.stdin.fileno(), packet[12:])
                except (BlockingIOError, BrokenPipeError):
                    pass
            return None
        if kind == 2 and stream == 24 and len(packet) == 12:
            self.last_mic = time.monotonic()
            if self.recorder is None and self.last_mic >= self.retry["recorder"]:
                self.recorder = self.start('pw-record', playback=False)
            if self.recorder and self.recorder.poll() is None:
                try:
                    self.pending.extend(os.read(self.recorder.stdout.fileno(), 8192))
                except BlockingIOError:
                    pass
            # Bound latency; absent host input corresponds to a silent mic.
            if len(self.pending) > 8192:
                del self.pending[:-4096]
            data = bytes(self.pending[:1024])
            del self.pending[:1024]
            self.microphone_blocks += 1
            return packet[:4] + bytes([3, 24]) + packet[6:12] + data.ljust(1024, b'\0')
        return None

    def start(self, command, playback):
        try:
            log = open('/tmp/mhi2-audio-host.log', 'ab')
            p = subprocess.Popen([command, '--raw', '--rate=48000', '--channels=1',
                                  '--format=s16', '-'],
                                 stdin=subprocess.PIPE if playback else subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL if playback else subprocess.PIPE,
                                 stderr=log)
            log.close()
            os.set_blocking((p.stdin if playback else p.stdout).fileno(), False)
            print('PCM host:', command, 'started', flush=True)
            return p
        except OSError as exc:
            print('PCM host unavailable:', command, str(exc), flush=True)
            self.retry["player" if playback else "recorder"] = time.monotonic() + 10
            return None

    def tick(self):
        for attr in ('player', 'recorder'):
            p = getattr(self, attr)
            if p is not None and p.poll() is not None:
                print('PCM host:', attr, 'exited', p.returncode, flush=True)
                if p.stdin: p.stdin.close()
                if p.stdout: p.stdout.close()
                setattr(self, attr, None)
                self.retry[attr] = time.monotonic() + 10
        if self.recorder is not None and time.monotonic() - self.last_mic > 2:
            self.recorder.terminate()
            self.recorder.wait(timeout=2)
            self.recorder.stdout.close()
            self.recorder = None
            self.pending.clear()

    def close(self):
        for p in (self.player, self.recorder):
            if p is not None and p.poll() is None:
                p.terminate()
                try: p.wait(timeout=2)
                except subprocess.TimeoutExpired: p.kill(); p.wait()
        if self.log: self.log.close()

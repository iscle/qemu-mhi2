#!/usr/bin/env python3
"""Decode the native RCC PCIe ISO TX2 stream into the cluster's video surface.

Input is little-endian length-prefixed native ISO blocks from QEMU's mostvideo
chardev. Only synchronized 188-byte MPEG-TS packets are passed to FFmpeg; the
block's driver headers are not part of the video stream.
"""
import argparse
import os
from pathlib import Path
import signal
import struct
import subprocess
import time


class TransportStream:
    def __init__(self):self.continuity={};self.packets=0;self.discontinuities=0
    def extract(self,block):
        start=0;packets=[]
        while start+188<=len(block):
            if block[start]!=0x47:
                start+=1;continue
            # Require a second sync word when there is room for another packet.
            if start+376<=len(block) and block[start+188]!=0x47:
                start+=1;continue
            packet=block[start:start+188];start+=188
            if packet[1]&0x80:continue  # transport error indicator
            afc=(packet[3]>>4)&3
            if afc==0:continue
            pid=((packet[1]&31)<<8)|packet[2];cc=packet[3]&15
            if afc&1 and pid!=0x1fff:
                old=self.continuity.get(pid)
                if old is not None and cc not in (old,(old+1)&15):self.discontinuities+=1
                self.continuity[pid]=cc
            packets.append(packet);self.packets+=1
        return b''.join(packets)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,default=Path('/tmp/mhi2-most-blocks.bin'))
    parser.add_argument('--output',type=Path,default=Path('/tmp/mhi2-cluster.ppm'))
    args=parser.parse_args()
    running=True
    def stop(signum,frame):
        nonlocal running
        running=False
    signal.signal(signal.SIGTERM,stop)
    signal.signal(signal.SIGINT,stop)
    decoder=None;stream=TransportStream();pending=bytearray();source=None
    def finish_decoder():
        nonlocal decoder
        if decoder is None:return
        try:decoder.stdin.close()
        except BrokenPipeError:pass
        try:decoder.wait(timeout=3)
        except subprocess.TimeoutExpired:
            decoder.terminate()
            try:decoder.wait(timeout=3)
            except subprocess.TimeoutExpired:decoder.kill();decoder.wait()
        decoder=None
    try:
        while running:
            if source is None:
                try:source=args.input.open('rb')
                except FileNotFoundError:time.sleep(.1);continue
            data=source.read(65536)
            if not data:
                # A slow renderer can pause between frames. Keep SPS/PPS and
                # reference pictures until shutdown; restarting here loses the
                # H.264 state and rejects subsequent inter-coded pictures.
                time.sleep(.05);continue
            pending.extend(data)
            while len(pending)>=4:
                n=struct.unpack_from('<I',pending)[0]
                if n<4 or n>1024*1024:raise ValueError('Invalid MOST block length')
                if len(pending)<4+n:break
                block=bytes(pending[4:4+n]);del pending[:4+n]
                ts=stream.extract(block)
                if not ts:continue
                if decoder is None:
                    decoder=subprocess.Popen(['ffmpeg','-hide_banner','-loglevel','warning',
                        '-threads','1','-flags','low_delay',
                        '-probesize','65536','-analyzeduration','100000','-fpsprobesize','0',
                        '-f','mpegts','-i','pipe:0',
                        '-an','-fps_mode','passthrough','-f','image2','-update','1','-atomic_writing','1',
                        '-y',str(args.output)],stdin=subprocess.PIPE)
                decoder.stdin.write(ts);decoder.stdin.flush()
    finally:
        if source:source.close()
        finish_decoder()
        print(f'MOST sink: {stream.packets} TS packets, {stream.discontinuities} continuity gaps',flush=True)


if __name__=='__main__':main()

"""Loopback component around the shared qemu-mhi2 ``rcc_peer.Peer``.

The caller owns QEMU startup and connects its TCP chardev/control clients to
these loopback listeners. This module is not an ordinary-launcher replacement.
"""
from __future__ import annotations

import errno
import importlib
import json
from pathlib import Path
import select
import socket
import sys
import threading
from typing import BinaryIO

from native_transport import FrameDecoder


_PEER_FACTORY_LOCK = threading.RLock()


class AudioDisabled:
    """No-op endpoint installed before the common Peer can process packets."""

    def process(self, packet: bytes):
        return None

    def tick(self) -> None:
        pass

    def close(self) -> None:
        pass


def loopback_listener() -> socket.socket:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    return listener


def create_shared_peer(mhi2_tools: str | Path, sock: socket.socket, capture: BinaryIO):
    """Instantiate the supplied common Peer with its host-audio path gated off.

    Upstream ``Peer`` imports ``AudioEndpoint``. That endpoint starts device
    subprocesses lazily while processing audio packets and uses POSIX-oriented
    device commands/paths. Replace only the module's endpoint factory during
    construction, so the common Peer retains all RCC protocol/service logic.
    """
    tools_path = str(Path(mhi2_tools).resolve())
    if tools_path not in sys.path:
        sys.path.insert(0, tools_path)
    common = importlib.import_module("rcc_peer")
    expected_module = (Path(tools_path) / "rcc_peer.py").resolve()
    if Path(common.__file__).resolve() != expected_module:
        raise RuntimeError(f"Imported RCC peer does not match caller path: {common.__file__}")
    with _PEER_FACTORY_LOCK:
        original_factory = common.AudioEndpoint
        common.AudioEndpoint = AudioDisabled
        try:
            peer = common.Peer(sock, capture)
        finally:
            common.AudioEndpoint = original_factory
    if not isinstance(peer.audio, AudioDisabled):
        raise RuntimeError("Shared RCC Peer did not receive the disabled audio endpoint")
    return peer


class RccPeerAdapter:
    """Pump framed Ethernet and newline-delimited controls for a common Peer."""

    def __init__(
        self,
        ethernet: socket.socket,
        peer,
        control: socket.socket | None = None,
        *,
        tcp_low_delay: bool = False,
    ) -> None:
        self.ethernet = ethernet
        self.control = control
        self.peer = peer
        self.tcp_low_delay = tcp_low_delay
        self.decoder = FrameDecoder()
        self.controls = bytearray()
        self.expected_close = False
        self.control_events = 0
        self.received_frames = 0

    def run(self) -> dict[str, object]:
        if self.tcp_low_delay:
            self.ethernet.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if self.control is not None:
                self.control.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            while True:
                self.peer.tick()
                readers = [self.ethernet]
                if self.control is not None:
                    readers.append(self.control)
                ready, _, _ = select.select(readers, [], [], 0.05)
                if self.control is not None and self.control in ready:
                    data = self.control.recv(4096)
                    if not data:
                        self.control.close()
                        self.control = None
                        self.controls.clear()
                    else:
                        self.controls.extend(data)
                        if len(self.controls) > 16384:
                            raise ValueError("Control line exceeds 16 KiB")
                        while b"\n" in self.controls:
                            line, _, remainder = self.controls.partition(b"\n")
                            self.controls = bytearray(remainder)
                            event = json.loads(line)
                            if event.get("type") == "host-expect-close":
                                self.expected_close = True
                            else:
                                self.peer.input_event(event)
                                self.control_events += 1
                if self.ethernet not in ready:
                    continue
                try:
                    data = self.ethernet.recv(65536)
                except ConnectionResetError:
                    if not self.expected_close:
                        raise
                    data = b""
                if not data:
                    self.decoder.finish()
                    if not self.expected_close:
                        raise EOFError("Unexpected QEMU RCC connection close")
                    break
                for frame in self.decoder.feed(data):
                    self.peer.receive(frame)
                    self.received_frames += 1
            return {
                "status": "CLOSED",
                "expected_close": self.expected_close,
                "received_frames": self.received_frames,
                "control_events": self.control_events,
                "complete_frame_boundary": not self.decoder.pending,
                "tcp_low_delay_requested": self.tcp_low_delay,
            }
        finally:
            self.peer.audio.close()
            for connection in (self.ethernet, self.control):
                if connection is not None:
                    connection.close()


def main() -> None:
    """Provide loopback ports only; session orchestration remains caller-owned."""
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mhi2-tools", type=Path, required=True)
    parser.add_argument("--capture", type=Path, required=True)
    args = parser.parse_args()
    if not (args.mhi2_tools / "rcc_peer.py").is_file():
        parser.error("--mhi2-tools must contain the common rcc_peer.py")
    if args.capture.exists():
        parser.error("Capture path must be new")
    ethernet_listener = loopback_listener()
    control_listener = loopback_listener()
    print(json.dumps({
        "host": "127.0.0.1",
        "ethernet_port": ethernet_listener.getsockname()[1],
        "control_port": control_listener.getsockname()[1],
        "audio": "disabled",
        "capture": str(args.capture.resolve()),
        "status": "LISTENING",
    }), flush=True)
    ethernet = control = peer = None
    try:
        ethernet, _ = ethernet_listener.accept()
        control, _ = control_listener.accept()
        args.capture.parent.mkdir(parents=True, exist_ok=True)
        with args.capture.open("xb") as capture:
            peer = create_shared_peer(args.mhi2_tools, ethernet, capture)
            result = RccPeerAdapter(ethernet, peer, control).run()
        print(json.dumps(result), flush=True)
    finally:
        for item in (ethernet_listener, control_listener):
            item.close()
        for item in (ethernet, control):
            if item is not None:
                item.close()
        if peer is not None:
            peer.audio.close()


if __name__ == "__main__":
    main()

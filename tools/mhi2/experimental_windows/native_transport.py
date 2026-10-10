"""Small, OS-aware primitives for experimental native host components."""
from __future__ import annotations

import os
from pathlib import Path
import struct


class FrameDecoder:
    """Decode BE-u32-length-prefixed Ethernet frames from a TCP byte stream."""

    MAX_FRAME = 65535
    MAX_BUFFER = 2 * (MAX_FRAME + 4)

    def __init__(self) -> None:
        self.pending = bytearray()

    def feed(self, data: bytes) -> list[bytes]:
        if len(self.pending) + len(data) > self.MAX_BUFFER:
            raise ValueError("Frame receive buffer exceeded bound")
        self.pending.extend(data)
        frames = []
        while len(self.pending) >= 4:
            length = struct.unpack_from("!I", self.pending)[0]
            if not 0 < length <= self.MAX_FRAME:
                raise ValueError(f"Invalid Ethernet frame length: {length}")
            if len(self.pending) < length + 4:
                break
            frames.append(bytes(self.pending[4 : 4 + length]))
            del self.pending[: 4 + length]
        return frames

    def finish(self) -> None:
        if self.pending:
            raise ValueError("Truncated framed stream")


class SessionLock:
    """Nonblocking one-byte file lock; the harmless lock file is retained."""

    def __init__(self, path: str | os.PathLike[str]) -> None:
        self.path = Path(path)
        self.file = None

    def acquire(self) -> "SessionLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.file = self.path.open("a+b")
        if self.path.stat().st_size == 0:
            self.file.write(b"\0")
            self.file.flush()
        self.file.seek(0)
        try:
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(self.file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            self.file.close()
            self.file = None
            raise RuntimeError("Native emulator session already owns this media") from exc
        return self

    def close(self) -> None:
        if self.file is None:
            return
        try:
            self.file.seek(0)
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(self.file.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.file.fileno(), fcntl.LOCK_UN)
        finally:
            self.file.close()
            self.file = None

    def __enter__(self) -> "SessionLock":
        return self.acquire()

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()
"""Loopback adapter for the existing native GL host stdio protocol.

This is a reusable component, not a QEMU launcher or a complete session runner.
"""
from __future__ import annotations

import asyncio
import os
from pathlib import Path
import socket
import struct


class RecordBoundary:
    """Track little-endian opcode/size records without changing their bytes."""

    MAX_RECORD = 64 * 1024 * 1024

    def __init__(self) -> None:
        self.header = bytearray()
        self.remaining = 0
        self.records = 0
        self.opcodes: dict[str, int] = {}

    def feed(self, data: bytes) -> None:
        offset = 0
        while offset < len(data):
            if self.remaining:
                amount = min(self.remaining, len(data) - offset)
                self.remaining -= amount
                offset += amount
                continue
            amount = min(8 - len(self.header), len(data) - offset)
            self.header.extend(data[offset : offset + amount])
            offset += amount
            if len(self.header) == 8:
                opcode, size = struct.unpack("<II", self.header)
                if size > self.MAX_RECORD:
                    raise ValueError("GL record exceeds 64 MiB bound")
                key = str(opcode) if opcode < 256 else "other"
                self.opcodes[key] = self.opcodes.get(key, 0) + 1
                self.header.clear()
                self.remaining = size
                self.records += 1

    def eof(self) -> None:
        if self.header or self.remaining:
            raise EOFError("Truncated graphics record at close")


class NativeGraphicsPeer:
    """Bridge QEMU's GL TCP stream to an explicitly selected host executable."""

    def __init__(
        self,
        executable: str | os.PathLike[str],
        shader_cache: str | os.PathLike[str],
        output: str | os.PathLike[str],
        *,
        env: dict[str, str] | None = None,
        encoder_executable: str | os.PathLike[str] | None = None,
        main_viewport: tuple[int, int] | None = None,
        tcp_low_delay: bool = False,
    ) -> None:
        self.executable = Path(executable).resolve()
        self.shader_cache = Path(shader_cache).resolve()
        self.output = Path(output).resolve()
        self.env = dict(os.environ if env is None else env)
        self.encoder_executable = (
            Path(encoder_executable).resolve() if encoder_executable is not None else None
        )
        if main_viewport is not None and (
            len(main_viewport) != 2
            or any(type(value) is not int or not 1 <= value <= 2048 for value in main_viewport)
        ):
            raise ValueError("Invalid native main display geometry")
        self.main_viewport = main_viewport
        self.tcp_low_delay = tcp_low_delay
        self.process = None
        self.server = None
        self.task = None
        self.writer = None
        self.stderr = None
        self.expected_close = False
        self.error = None
        self.boundary = RecordBoundary()
        self.received_bytes = 0
        self.replied_bytes = 0

    async def start(self) -> dict[str, int]:
        if not self.executable.is_file():
            raise FileNotFoundError(self.executable)
        if not self.shader_cache.is_dir():
            raise FileNotFoundError(f"Shader cache directory does not exist: {self.shader_cache}")
        self.output.mkdir(parents=True, exist_ok=False)
        self.env.update(
            MHI2_GL_FRAME=str(self.output / "frame.ppm"),
            MHI2_SHADER_CACHE=str(self.shader_cache),
        )
        if self.encoder_executable is not None:
            self.env["MHI2_ENCODER_EXE"] = str(self.encoder_executable)
        if self.main_viewport is not None:
            self.env.update(
                MHI2_GL_MAIN_WIDTH=str(self.main_viewport[0]),
                MHI2_GL_MAIN_HEIGHT=str(self.main_viewport[1]),
            )
        self.stderr = (self.output / "host.stderr.bin").open("xb")
        self.process = await asyncio.create_subprocess_exec(
            str(self.executable),
            env=self.env,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=self.stderr,
            limit=65536,
        )
        self.server = await asyncio.start_server(self.accept, "127.0.0.1", 0, limit=65536)
        return {"pid": self.process.pid, "port": self.server.sockets[0].getsockname()[1]}

    async def accept(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        if self.task is not None:
            writer.close()
            await writer.wait_closed()
            return
        self.task = asyncio.current_task()
        self.writer = writer
        connection = writer.get_extra_info("socket")
        if self.tcp_low_delay:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.tcp_nodelay = connection.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY)
        self.server.close()

        async def receive() -> None:
            while True:
                try:
                    data = await reader.read(65536)
                except ConnectionResetError:
                    if not self.expected_close:
                        raise
                    data = b""
                if not data:
                    self.boundary.eof()
                    if not self.expected_close:
                        raise EOFError("Unexpected QEMU graphics close")
                    break
                self.boundary.feed(data)
                self.received_bytes += len(data)
                self.process.stdin.write(data)
                await self.process.stdin.drain()
            self.process.stdin.close()
            await self.process.stdin.wait_closed()

        async def transmit() -> None:
            while data := await self.process.stdout.read(65536):
                self.replied_bytes += len(data)
                writer.write(data)
                await writer.drain()
            if not self.expected_close:
                raise EOFError("Unexpected GL host stdout close")

        pumps = [asyncio.create_task(receive()), asyncio.create_task(transmit())]
        try:
            await asyncio.gather(*pumps)
        except (OSError, EOFError, ValueError) as exc:
            self.error = repr(exc)
        finally:
            for pump in pumps:
                if not pump.done():
                    pump.cancel()
            await asyncio.gather(*pumps, return_exceptions=True)
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass

    def assert_alive(self) -> None:
        if self.error:
            raise RuntimeError("Native GL bridge: " + self.error)
        if self.process is not None and self.process.returncode is not None:
            raise RuntimeError("Native GL host exited unexpectedly")

    async def close(self) -> dict[str, object]:
        self.expected_close = True
        if self.server is not None:
            self.server.close()
        if self.task is not None:
            try:
                await asyncio.wait_for(asyncio.shield(self.task), 5)
            except asyncio.TimeoutError:
                self.error = self.error or "Graphics TCP drain timed out"
                self.task.cancel()
                await asyncio.gather(self.task, return_exceptions=True)
        if self.writer is not None:
            self.writer.close()
            try:
                await asyncio.wait_for(self.writer.wait_closed(), 5)
            except (OSError, asyncio.TimeoutError):
                pass
        if self.server is not None:
            await asyncio.wait_for(self.server.wait_closed(), 5)
        if self.process is not None:
            if self.process.stdin is not None and not self.process.stdin.is_closing():
                self.process.stdin.close()
            try:
                await asyncio.wait_for(self.process.wait(), 10)
            except asyncio.TimeoutError:
                self.error = self.error or "GL host did not exit after stdin EOF"
                self.process.terminate()
                try:
                    await asyncio.wait_for(self.process.wait(), 5)
                except asyncio.TimeoutError:
                    self.process.kill()
                    await asyncio.wait_for(self.process.wait(), 5)
        if self.stderr is not None:
            self.stderr.close()
        return {
            "pid": self.process.pid if self.process else None,
            "exit_code": self.process.returncode if self.process else None,
            "error": self.error,
            "received_bytes": self.received_bytes,
            "replied_bytes": self.replied_bytes,
            "records": self.boundary.records,
            "opcodes": dict(self.boundary.opcodes),
            "complete_record_boundary": not self.boundary.header and not self.boundary.remaining,
            "tcp_low_delay_requested": self.tcp_low_delay,
            "adapter_TCP_NODELAY": getattr(self, "tcp_nodelay", None),
        }
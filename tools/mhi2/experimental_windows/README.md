# Experimental native Windows components

These source-only modules collect reusable pieces from the Research Windows integration: a BE-u32 Ethernet stream decoder and cross-platform session lock, a loopback GL host adapter, and an RCC socket adapter around the fork's existing `tools/mhi2/rcc_peer.py` `Peer` implementation.

They are components, not a complete launcher or ordinary `launch_ui.py` redesign. A future session runner still needs to own QEMU startup, configure its TCP endpoints, connect the RCC and graphics peers, manage the lock over the full media-owning session, wire input/control events, and coordinate ordered shutdown. The GL peer owns only the explicitly supplied GL host child; it does not claim process-tree supervision. The RCC helper exposes ports and consumes caller connections but does not launch QEMU.

For graphics, callers must provide the executable and shader-cache directory explicitly. The adapter inherits the caller environment by default and does not set a guessed MSYS2 `PATH` or executable location. An optional encoder path is passed only when the caller provides one.

The RCC adapter imports the shared `rcc_peer.Peer` from a caller-supplied `tools/mhi2` directory. Upstream `Peer` imports `AudioEndpoint`; the current endpoint starts `pw-play`/`pw-record` or `sox` lazily while processing packets and writes to POSIX-style `/tmp` paths. The adapter temporarily replaces only that constructor factory while creating `Peer`, so all host-audio processing is explicitly disabled before packet handling. Audio behavior remains unimplemented and untested here.

The modules contain no private evidence manifests, source hash admission, OEM/corpus/coding/adaptation/language fixture imports, profile-overlay gate, or frozen host paths. They do not make any firmware-support claim.

Run the firmware-free checks with:

```powershell
python -m unittest discover -s tests -v
```

The checks cover synthetic framing, malformed/truncated streams, and cross-process lock exclusion and reacquisition. They do not test a QEMU launch, graphics host, RCC service behavior, audio, or vehicle firmware.
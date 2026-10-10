# Experimental Windows host and SKG13 components

This draft collects source-only experiments against `iscle/qemu-mhi2` main at
`78b8cb2d34d7c08f34fc33c0cab1ab960edfc000`. It is intended for maintainer feedback
on scope and contribution eligibility. The implementation and preparation used
AI assistance, including Luna assistants and a separate Sol review. The inherited
AGENTS/provenance restriction remains acknowledged; no maintainer exception or
DCO certification is claimed.

The changes cover:

- [Native Windows GL/encoder build instructions](../../WINDOWS_BUILD.md), using
  MSYS2 MinGW64, ANGLE and libuv, plus an explicit build-only install-link opt-out.
- Experimental SKG13 P4526/MU1440 profile selection, 1280x640 main geometry,
  coordinate conversion at the RCC wire boundary, and viewer label encoding.
- [Reusable native session components](experimental_windows/README.md), with
  loopback transport, session locking and explicitly disabled host audio.
- Windows sparse copying and an app-partition capacity check before preparation
  creates output. Capacity refusal preserves source and existing output. No
  automatic partition relocation or larger-layout creation is implemented.

The ordinary `launch_ui.py` remains POSIX-oriented. The native adapters need a
future shared runner to own QEMU startup, endpoints and complete session lifetime.
The normal builder explicitly refuses the experimental SK profile before writing
media; bootstrap compatibility and a separately validated larger layout remain
prerequisites. The 1280x480 cockpit size is a requested geometry, without verified
guest MOST video.

Historical private Windows integration on the pinned upstream plus additional
private preparation/configuration rendered VW K3342/MU1427 at 800x480 and SKG13
P4526/MU1440 at 1280x640. Both runs were capped at 600 seconds, showed real guest
input effects and verified clean shutdown and unchanged media. These observations
do not establish boot acceptance of this sanitized public diff. Navigation was
unready (SK showed "Navigation database not available"), audio was disabled, and
MOST video was unverified. No fresh Linux/macOS or physical-unit acceptance exists.

Firmware-free checks from the repository root:

```sh
python tools/mhi2/check_windows_preparation.py
python -m unittest discover -s tools/mhi2/tests -p 'test_skoda_*candidate.py' -v
python -m unittest discover -s tools/mhi2/experimental_windows/tests -v
```

Viewer checks require PySide6 and select the offscreen Qt platform. The preparation
checks require the existing Python cryptography dependency; Windows sparse checks
require a filesystem supporting sparse files. Tests cover synthetic data only.
Graphics build and encoder validation are described separately, including known
frame drops; compile success is not full rendering or encoder throughput acceptance.

No firmware, maps, captures, OEM binary payloads, credentials, real unit identifiers
or private research history are included. Users must obtain and prepare their own
eligible media and shader cache independently.

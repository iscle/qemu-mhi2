# Virtual Cockpit video over the RCC MOST interface

Start with the existing command:

```sh
bash /home/iscle/qemu-mhi2/tools/mhi2/run_ui.sh
```

The **Virtual Cockpit** button opens a separate video window. It shows received
H.264 video; while the firmware sends none it displays a waiting message.
This is an emulated video sink, not an instrument-cluster firmware or a complete
cluster with gauges. The firmware produces native navigation video through this
path. Map rendering remains incomplete because some captured Tegra shaders are still unsupported.
Opening the cockpit window requests the native map and display-management
interfaces and retries while navigation starts; no extra shell command is needed.

## Implemented path

Guest producer → original `devp-iso-mmx-mib2` → RCC PCIe BAR3 mailbox → DMA →
channel 3 ISO TX2 receiver → MPEG-TS → FFmpeg H.264 decoder → cockpit window.

The launcher starts the original QNX driver with its native 188-byte packet,
64-packet block configuration. QNX normally waits for the remote RCC Qnet device
before starting it; this local experiment starts the driver directly because
that remote resource manager is not emulated. No guest MOST driver is replaced.

The endpoint preserves a reserved BAR2 so QNX's compact active-BAR table places
BAR3 at the index expected by the native driver. BAR3 is a 4 KiB register bank:
channel command at `4*c`, DMA address at `4*(c+8)`, length at `4*(c+16)`.
Writing `c+2` to BAR0+0x54 rings the doorbell. Command 0x22222222 transmits a block.
The model copies through the programmed outbound DMA window, clears the native
completion word and raises the channel MSI. Transfers are limited to 1 MiB and
cannot cross an 8 MiB DMA window. RX command 0x11111111 is not implemented.

Channel 3 blocks are exported to the optional `mostvideo` chardev as a
little-endian uint32 length followed by the native bytes. `most_sink.py` removes
any non-TS prefix, validates packet synchronization and monitors continuity.
It writes decoded frames atomically, uses low-delay single-thread decoding,
and retains H.264 parameter sets and reference pictures across input pauses.
Buffered frames are flushed when the session stops. Other TX channels are acknowledged but discarded.
The launcher saves MOST logs and captured blocks with the session diagnostics.

## Evidence

`check_devices.py` exercises mailbox sizing, DMA completion, channel MSI and
oversized-transfer rejection. `check_services.py` checks TS synchronization,
header removal and continuity tracking. An ARM QNX test client using the real
`open`, `devctl(0x80040509)` and `write` APIs transmitted two 12032-byte blocks
through the original driver. The captured 128 TS packets had zero continuity
gaps and decoded successfully to an 800×400 test image. A repeated live test
sent 16 blocks: all 192512 payload bytes matched the input exactly, and the
running receiver produced an 800×400 frame. Replaying the same finite stream
resets its timestamps/counters, so FFmpeg reports discontinuities at replay
boundaries. `most-test.json` and `most-live-*` retain that evidence.

Evidence is in `/home/iscle/Downloads/mhi2-analysis/qemu/evidence-ui/`:
`mhi2-most-blocks.bin`, `mhi2-native-captured.ts`, `mhi2-native-captured.png`.
This is a transport test using a generated blue video, not proof of the native
navigation producer. The driver is analyzed in the existing
`MHI2_Quickboot_DeepAnalysis` Ghidra project; exported analysis lives under
`qemu/drivers/most-iso-analysis/`.

## Remaining dependencies

The simulated diagnostic profile selects MOST streaming and H.264 using the
firmware's DashboardDisplayConfig fields 1172 and 171. Cover-art and call-picture
capabilities remain disabled: these require the separate DSIKombiPictureServer
service, which is not supplied. That service does not produce navigation video.

The experimental EGL/GLES replacements publish native CWM surfaces and import
shared NvRm images, so the original display manager can compose them. A third
replacement, `libnvvsenc.so`, forwards the firmware's encoder input images to
host FFmpeg with VA-API H.264 when available (OpenH264 fallback). The original display manager still selects the frames,
queues the resulting MPEG-TS, and writes to the original MOST driver. These
replacements live in generated emulator media; the supplied firmware archive
and extracted original libraries are preserved. Successful host encoder tests
do not establish that the native navigation producer is working.

The supplied P470 map package can be staged with `prepare_maps.py`; extract
both Mib1 and Mib2 because MIB2's content manifests reference shared MIB1 files.
The script follows the MIB2 installation destinations and builds a separate
QNX6 map-enabled eMMC image. It preserves map payloads and signatures. The
launcher automatically selects `emmc-maps.raw` when present; `MHI2_EMMC` can
override the disk. The native navigation core accepts this database and reports itself fully
operable. Native cockpit frames have been decoded from the resulting stream. Full MOST control messages, ring/network management, receive
channels, DTCP and instrument-cluster CAN/BAP behavior are not modeled.
The transport work does not make those functions operational.

References used alongside firmware disassembly:

- [VcMOSTRenderMqb](https://github.com/OneB1t/VcMOSTRenderMqb): original driver startup and cluster display selection.
- [mhi2_altscreen_carplay](https://github.com/harman-f/mhi2_altscreen_carplay): MPEG-TS transmission in 64×188-byte blocks.
- [mhi2-android-auto-video-vc](https://github.com/chopinwong01/mhi2-android-auto-video-vc): native cluster video paths.

Navigation maps and startup (2026-09-30): `prepare_maps.py` installed the supplied
P470 package into `emmc-maps.raw`, including shared Mib1 payloads referenced by
Mib2 manifests. A native trace confirmed EU/VW coding with navigation enabled,
but `NavigationAppEnabledData` was absent in persistence partition 1211. After
adding that one-byte preference through `dumb_persistence_writer`, the original
navigation application scanned the database, registered map viewer instance 0,
and created displayable 33 (`DISPLAYABLE_KOMBI_MAP_VIEW`). The native MOST
encoder then opened. These are startup milestones, **not verified cockpit
video**: additional precompiled navigation shaders are still unsupported and
the captured navigation surface is blank.

`navigation_persistence.py` documents and reproduces the TOC serialization while
preserving existing entries. `navigation-enabled-persist.bin` is a captured
8 MiB guest-written flash filesystem containing English defaults plus the
navigation preference; the media builder prefers this seed when present. No
map signature/FEC checks are changed. Original captures are under
`qemu/navigation-capture` in the analysis directory. Navigation startup and
persistence functions are being named in the existing shared Ghidra project.

The RCC emulator now registers native `asi.videomanagement.Connection` instance
1 (UUID `45926ef4-737d-44b0-aacd-b24a0671e42d`). Display management requests
terminal 0, label 3 before submitting encoder frames. The factory's wire methods
are 9=request, 8=release, 10=response, 4=requestFrameRate, 7=updateFrameRate;
arguments are big-endian int32 without optional wrappers. Wire status 0 means
ACTIVE, 1 STOPPED, 2 ERROR. Display management maps ACTIVE to its different
internal state value 1. Unsupported terminal/label pairs return ERROR. These
methods and the native callback are annotated in the shared Ghidra project.

The map image builder marks the unused tail of the last allocation-bitmap block
as allocated, as required by the native filesystem checker. The experimental
map disk was repaired using the original `chkqnx6fs` while unmounted. The
host graphics bridge also forwards active attribute/uniform reflection and
reports each native window's dimensions; ten captured navigation shaders
have explicit translations. Many additional navigation shaders remain
unsupported. These changes are still undergoing native video validation.

Native video milestone (2026-09-30): display management selected context 70 /
displayable 33, submitted its 800x480 EGLImage, and the original ISO driver
transmitted host-encoded H.264 through the emulated MOST endpoint. One completed
run contained 103808 TS packets with zero continuity gaps. Decoded native map
geometry and a corresponding stream capture are in
`qemu/evidence-native-vc/` under the analysis directory. This is native producer
evidence, unlike the earlier generated transport fixture. The initial geometry
was visibly incomplete; a complete map renderer is not claimed.

The encoder initializes its own NvRm dispatch table: image creation may resolve
through the GLES DSO while the encoder entry resolves through EGL. Relying on
the other DSO's private initialization caused a null function-pointer crash on
the first native input image. The host also identifies displaymanager's physical
window so navigation-client swaps cannot replace the head-unit viewer image.

The desktop cockpit control is verified from a clean boot, including an early
request before the navigation service exists. Unanswered service requests retry on the same accepted connection; video
submission waits for native readiness and visibility notifications. Later captures show native roads
after the fog and extruded-road shader translations were added. Labels and
other unsupported material passes are still missing. The decoder no longer
restarts after a two-second pause: that discarded SPS/PPS state and caused
inter-coded frames to fail until the next keyframe.

Latest saved validation: `qemu/evidence-native-vc/validation.json` records 678
complete native blocks (43392 TS packets), zero continuity gaps, and zero errors
from the corrected decoder. `native-roads.png` is the decoded image.
`check_most_decoder.py CAPTURE` reproduces the 3.5-second pause regression check.
The headless test instance was stopped after validation; launch normally with
the single command above, select NAV, and open Virtual Cockpit.


The current encoder bridge uses bounded asynchronous workers. Opcode 127
submits a copied image and returns queue status immediately; opcode 126 polls
complete 188-byte TS packets. A guest receiver thread delivers them through the
original encoder callback, outside the shared graphics transport lock. Empty
polls sleep for 20 ms and do not switch the active host GL context. Close
joins that receiver before releasing its callback state. Host codec startup,
encoding and shutdown run on worker threads. Each encoder holds at most two
waiting raw images, one in flight and 4 MiB of output; overload drops new images
instead of growing the queue. Partial TS packets are retained until complete,
so there is no per-frame 40 ms quiet-period heuristic.

The guest caps submissions to the encoder's configured frame rate before any
pixel copy, without sleeping in the display manager. Contiguous EGLImages use
32 KiB NvRm reads and host row flipping (47 calls for an 800x480 frame, instead
of 480). Padded surfaces retain row reads. Pixels still cross guest memory;
this is not a zero-copy GPU path. FFmpeg CPU/filter threads are limited to one,
including format conversion before VA-API upload.

The launcher probes accessible `/dev/dri/renderD*` devices with a real H.264
VA-API encode, selecting `h264_vaapi` and hardware upload when successful. It
prints the selected device. `MHI2_ENCODER=vaapi` requires acceleration and fails
if probing fails; `MHI2_VAAPI_DEVICE=/dev/dri/renderD129` selects a particular
node. Default `auto` reports an explicit asynchronous software fallback if no
hardware backend works, with details in `/tmp/mhi2-encoder-probe.log`.
`MHI2_ENCODER=software` is an explicit diagnostic override. These settings
change the host bridge, not the navigation/HMI executable or native MOST driver.

`python3 tools/mhi2/check_encoder.py` verifies a real codec round trip, TS
continuity and bounded submissions with a deliberately stalled codec. Hardware
probe selection/failure handling is tested separately from physical hardware;
the agent sandbox has no `/dev/dri`, so VA-API execution needs desktop validation.

The final asynchronous-encoder live check reached guest uptime 5:38 with
cockpit video enabled from approximately 2:18. It captured 998 native MOST
blocks (12,007,936 TS bytes); a finite copy ending before the last partial PES
decoded 934 frames with zero continuity errors and no decoder errors. NAV →
MENU → RADIO → NAV worked while cockpit video continued. There were no
navigation restarts, display-manager watchdog timeouts, or encoder-call
watchdog warnings in that run. The first changed MENU frame appeared after
1.78 seconds of wall time in the software-only sandbox; this is not a claim
of desktop GPU latency or a complete UI performance fix. Evidence and measured
limits are in `qemu/evidence-async-encoder/validation.json` under the analysis
workspace, alongside the captures and screenshots.

Initial asynchronous experiments exposed excess submissions (267 encoded
frames in roughly 27 guest seconds despite a 5 fps encoder setting). Moving
callbacks back to the original thread and restoring row reads did not prevent
navigation exits. The final run caps input cadence before copies, keeps
asynchronous callbacks, uses bounded read batches, and avoids encoder-related
GL context switches. Navigation and HMI code remain unchanged.

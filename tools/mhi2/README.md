# MHI2 Tegra3 emulation

Upstream QEMU `81ce3a87737aa50716c42db8886082d12783e0a1` was merged into
`mhi2-mainline-boot` in `/home/iscle/qemu-mhi2`. The original mounted checkout
and firmware captures have not been modified.

For a new computer, start with the [Audi A3 reproduction guide](AUDI_A3.md#reproduce-on-another-computer).
It covers dependencies, the private bootstrap download, building from the
original P5089 archive, and launching the native UI with rotary controls.
The workspace-specific commands below describe the older VW setup.

See [LSD diagnostic shells](DIAGNOSTIC_SHELLS.md) for the built-in COMM Doctor,
Java Trace Terminal, and production trace-server configuration.

## Run the prepared K3342 firmware

For the interactive UI, launch the prepared experimental media and viewer together:

```sh
bash /home/iscle/qemu-mhi2/tools/mhi2/run_ui.sh
```

The UI defaults to English. The viewer provides touch and the Volkswagen side
buttons and knobs. Close the viewer to stop the session. See
[RCC_INPUT.md](RCC_INPUT.md) for setup, evidence and limitations.

Cockpit encoding runs asynchronously. The launcher automatically probes VA-API
hardware H.264 and prints the selected backend. To require acceleration, run
`MHI2_ENCODER=vaapi bash tools/mhi2/run_ui.sh`; an unavailable encoder then stops
startup with a diagnostic. See [MOST.md](MOST.md) for device selection and tests.

For the native serial-console experiment:

```sh
/home/iscle/qemu-mhi2/tools/mhi2/run.sh \
  /home/iscle/Downloads/mhi2-analysis/qemu/k3342-70
```

UART D is the console. Ctrl-A C switches to the QEMU monitor; Ctrl-A X exits.
Both eMMC and NOR writes use temporary snapshots. The input images are preserved.
Set `MHI2_EMMC=/absolute/path/emmc-full.raw` to select the complete data disk.
For a serial log, set `MHI2_CONSOLE=file:/absolute/path/uart.log`.
Use `-d guest_errors -D /absolute/path/devices.log` as extra arguments for diagnostics.

## What has actually booted

The unmodified executable payloads from K3342 run through AVP Quickboot,
CPU Quickboot 17.37.03, authenticated primary stage2, QNX startup, kdumper,
procnto-smp-instr, and userspace drivers/scripts. The serial log in `evidence/`
includes the PMU setup message, the SDHCI driver,
`Starting /usr/sbin/startup.sh ...`, and the real IFS loader mounting the
73,400,320-byte main userspace image from NOR at `0x92000000`. OOC and capture
services also execute. These messages originate in the firmware.
The four Cortex-A9 cores reach QNX's idle WFI callout at `0xfc41202c`.

The current build mounts the NOR filesystems and the K3342 app partition,
starts the normal power path, and launches the Java HMI. The display controller
shows the captured Audi boot splash (the base NOR predates this Volkswagen
update). The separate GLES-forwarding media renders the Volkswagen radio UI
and accepts production RCC key-panel touch events.

The original prepared disk lacked the data partition layout expected by fstab.
`create_emmc.py` builds that layout with the exact app partition size. The local
`/home/iscle/Downloads/mhi2-analysis/qemu/emmc-full.raw` has all eleven blank
partitions formatted by the firmware's own `mkqnx6fs`; normal startup confirms
that they mount. Navigation/map data is not provided by those blank partitions.

Corrections since the first userspace milestone include the 600 MHz global timer,
eMMC power-up timing, T30 HIDREV, I2C packet mode, NOR 64-bit probe accesses,
writable NOR overlays, the normal-power GPIO/IOC frame, PLL lock bits, display
interrupts, SMMU page walks and HOST1X display/syncpoint command execution.
HOST1X channels use a 16 KiB stride. The first GR2D identity stretch-blit job now
executes and completes. Native GR3D commands remain unimplemented. Other observed
failures include the LVDS serializer and capture chip. The PCIe RCC endpoint
now exchanges Ethernet traffic through the original guest driver and MSI.

`glforward/` is a separate, incomplete GLES forwarding experiment adapted from
the original workspace prototype. It uses a host Mesa EGL pbuffer and an optional
MMIO transport enabled by a `glbridge` chardev. Its replacement EGL/GLES libraries
are installed only in `k3342-gl-debug-nor.bin` and `emmc-gl.raw`. It is not native
GPU emulation, and the prototype still has incomplete entry points and context
handling. The radio UI and touch navigation have been demonstrated with it;
missing vehicle and companion services still limit other applications.

## Rebuild

```sh
cd /home/iscle/qemu-mhi2
./configure --target-list=arm-softmmu --disable-docs --disable-sdl \
  --disable-gtk --disable-fuse --disable-werror --disable-download
ninja -C build -j12 qemu-system-arm
```

The local copy includes the previously available softfloat/testfloat/keycodemapdb
subprojects, so configuration does not need to fetch them.

## Reproduce the emulation images

`prepare.py` requires Python's `cryptography` package. Supply a new destination:

```sh
python3 tools/mhi2/prepare.py \
  --extracted /home/iscle/Downloads/mhi2-analysis/extracted \
  --base-nor /path/to/mmx_fs0_k2589 \
  --base-iram /path/to/tegra3_iram.bin \
  --base-emmc /path/to/emmc_v2.img \
  --output /path/to/new-media-directory
```

The captured partition geometry matches variant 70: 42 MiB MAIN_STAGE2 and
8 MiB PERSIST. Variant 50 has different partition sizes and is not silently
truncated to fit. The builder installs the archive's matching Quickboot,
primary/recovery QNX images, MAIN_STAGE2, SYSTEM, PERSIST and eMMC app filesystem.
Other eMMC partitions, splash and SWDL retain captured content. Input payload
hashes and provenance are saved in `manifest.json`.

The update images are not complete flash dumps. The harness reconstructs the
installer/BootROM handoff using the captured BIT/BCT. It normalizes the Android
image magic after checking its stored header CRC, pads stage2 for the CMAC,
validates the zero CMAC key against the original captured stage2 digests, then
updates those digests in the IRAM BCT for the new stage2. It marks the primary
loader successful in the BIT. No executable instruction is patched; the firmware
still computes and compares the CMAC. NOR-resident BCT data is unchanged.
These are emulator inputs, not images for flashing a physical headunit.
BootROM, hardware fuses and hardware-rooted secure boot are not emulated.

## Hardware corrections and references

Firmware paths are explicit (`-bios` and `-machine iram=`); reset reloads the
IRAM/shadow blobs and AVP entry. The unsupported low DRAM alias was removed:
Quickboot loads the QNX image at physical `0x80a00800`. UARTs now connect to
GIC SPIs 36, 37, 46 and 90. CAR CCLK_BURST_POLICY/OSC_CTRL use documented reset
values rather than zero-filled state. The upstream merge also updates device,
header and display APIs and removes unconditional stdout logging from the
shared unimplemented-device model.

Sources used:

* [Linux Tegra30 device tree](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/nvidia/tegra30.dtsi): memory map and UART interrupt wiring.
* [NVIDIA T30 register definitions](https://github.com/Project-Google-Tango/vendor_nvidia_jxd_src/tree/master/tegra/hwinc/t30): CAR reset values and peripheral layouts; also available in the original mib directory.
* Firmware disassembly in `/home/iscle/Downloads/mhi2-analysis/ghidra/`: boot addresses, BCT vendor CMAC records, BIT status, QNX startup and userspace scripts.

## Validation

```sh
python3 tools/mhi2/check_boot.py \
  /home/iscle/Downloads/mhi2-analysis/qemu/k3342-70 \
  --output /tmp/mhi2-checks
```

This checks the actual primary QNX header in guest RAM, observes its userspace
serial marker, resets the VM and checks the marker again. It then corrupts both
stage2 copies in a temporary NOR image and requires Quickboot's authentication
failure message. All checks passed; results and logs are under `evidence/`.
The short check intentionally needs no eMMC: the first userspace stage is in NOR.
The historical evidence directory predates the later storage and display fixes.
`check_devices.py MEDIA` exercises packet I2C, SMMU DMA, HOST1X channels 0/2,
syncpoints, a display gather, mlocks, and a GR2D pixel copy with completion.


## Companion services and Virtual Cockpit

The single-command launcher also starts the recovered RCC service models and
MOST video receiver. See [MISSING_MENU_DATA.md](MISSING_MENU_DATA.md) for version
and Car menu corrections, and [MOST.md](MOST.md) for the native ISO transport,
Virtual Cockpit window and remaining cluster-rendering dependencies.

Opening the cockpit during boot queues its video request. The RCC client
subscribes to native map readiness and visibility, asks for visibility only after
`updateReady(true, valid)`, and starts MOST video only after
`updateViewVisible(true, valid)`. Accepted service connections stay open while
navigation initializes; unanswered service requests retry on the same connection,
and repeated viewer refreshes do not reset it. This avoids
starting the encoder against a missing map displayable. Navigation/HMI binaries
and database checks remain unchanged. A cold boot with early cockpit requests
rendered both maps with no navigation initialization timeout, invalid-displayable
message, or native-agent EXIT failure. Logs and screenshots are saved in
`qemu/evidence-navigation-startup/` in the analysis directory.

## Speech resources and feature service

The launcher prefers `emmc-complete.raw` when its `.speech.json` metadata exists.
This disk includes the supplied navigation maps and the original K3342 speech
resources. Reproduce the additional speech partition from an extracted update:

```sh
python3 tools/mhi2/prepare_speech.py /path/to/extracted-update \
  /path/to/emmc-maps.raw /path/to/emmc-complete.raw
MHI2_EMMC=/path/to/emmc-complete.raw bash tools/mhi2/glforward/build_local.sh
```

The installer checks the firmware manifest's directory sizes, reserves free
blocks and inodes for generated speech data, and relocates subsequent logical
partitions without changing the input disk. The original speech executable,
TTS and recognition engines run in the guest. The RCC peer provides audio
connection request/fade/release notifications. No speech readiness notification
is fabricated and the HMI's original domain timeout remains unchanged.
`audio/pcm_endpoint.c` is an added guest companion service. The default hardware
path uses the Tegra30 APB DMA model, including PPCS SMMU translation and real
buffer transfers before completion interrupts. The original driver owns its
queues and performs TDM routing. The companion exchanges microphone input and
ANN1/ANN2 announcement output through an emulator-only AHUB mailbox, with bounded
8192-sample rings. This currently taps announcements, not every media output slot.
Streams use 512-frame blocks at 48 kHz, signed 16-bit LE. The old queue bridge is
retained as a fallback for builds without the mailbox. The guest service does not
replace or patch the speech executable or library.

`audio_host.py` connects those streams to PipeWire (`pw-play` / `pw-record`)
on Linux, with a SoX fallback, or to SoX/CoreAudio on macOS. Install SoX with
`brew install sox` on macOS. See [host parity](HOST_PARITY.md).
Microphone capture starts only while the native microphone queue is active and
stops after it becomes idle. The UDP endpoint at simulated RCC port 50000 is an
emulator transport, not a recovered production audio protocol. Logs are in
`/tmp/mhi2-audio-host.log`; outgoing native prompt PCM is captured in
`/tmp/mhi2-speech-output.s16le`. Run from the desktop user session for PipeWire
access. The agent sandbox denies PipeWire connections, so audible playback and
recognition against a real microphone cannot be validated there.

`check_audio.py` checks queue ownership, ordering, full/empty/inactive states,
bounds, and PCM packet handling. The original driver and library, with named
queue routines, are in the existing `MHI2_Quickboot_DeepAnalysis` Ghidra project.
Ordinary launcher sessions keep the RCC peer stable. Developer hot reload is
opt-in with `MHI2_RCC_RELOAD=1`, because reload disconnects native clients.

`rcc_features.py` supplies explicit simulated feature states through the native
MMX FEC service, including navigation, speech, Android Auto, CarPlay and
MirrorLink. This is a simulator fixture, not a signed activation container.
The diagnostic profile also enables the smartphone integration variants.
Feature permission alone does not implement USB phone transport, Bluetooth,
radio hardware or remote online services.

The RCC `FecManager.checkDataSignature` service hashes the supplied manifest
bytes and checks the RSA-1024 / SHA-1 PKCS#1 v1.5 signature using the public key
from the original RCC firmware. Invalid signatures return failure. The feature
fixture does not bypass map signature verification.

The optional graphics bridge now exposes a 2 MiB RAM staging area at
`0x5e000000` (1 MiB in each direction), with bounded doorbells at the existing
`0x5f000000` control page. This avoids one MMIO trap per four bytes of frame
transfer. Older guest libraries retain the bounded 4 KiB transport. Run
`check_gl_transport.py MEDIA` to check both paths and oversized requests, and
`check_services.py` for companion service protocol checks.

The translated compositor and navigation shaders use standard GLES2 blending
for Tegra's baked source-over operations, including destination-alpha
preservation for road fill. They do not require
`GL_EXT_shader_framebuffer_fetch`, which is unavailable on the desktop AMD
radeonsi driver. Each draw restores the guest's blend state, and blend metadata
is isolated per guest graphics client. The launcher uses the normal host driver
selection; it does not force software rendering.

`/tmp/mhi2-glhost --check-shaders` compiles all 17 known translations after the
launcher has built the helper. For pixel and state-restoration checks:

```sh
cc -O2 tools/mhi2/glforward/host/check_blending.c -o /tmp/mhi2-check-blending -lEGL -lGLESv2 -lm
/tmp/mhi2-check-blending
```


Earlier speech bring-up evidence is in
`/home/iscle/Downloads/mhi2-analysis/qemu/evidence-speech-startup/`.
Navigation DSI registration occurs around 2:41 guest uptime, versus the previous
4:57 after the speech timeout. Both native speech engines report ready around
3:00. The validation captures 21.664 seconds of nonzero native prompt PCM,
all 55 successful live map signature checks, and 18,304 native cockpit TS
packets without continuity gaps. `app-connect-enabled.png` shows the normal USB
connection screen with Android Auto, CarPlay and MirrorLink, rather than an
activation request. USB phone passthrough has not been implemented or tested.

The subsequent timeout fixes bring navigation's native `AS_RUNNING` request to
about 2:00 guest uptime (previously 2:41). This is a service-start milestone,
not a measurement of wall-clock time or when all map tiles finish loading.
Evidence is in `qemu/evidence-timeouts/` in the analysis directory.

The emulated IOC now delivers status at 100 ms, within the original driver's
300 ms receive deadline. MC hot-reset status reflects completed synchronous
flushes. Companion services provide native display brightness/power replies,
key-panel settings/property replies, radio/sound profile readiness and changes,
empty vehicle start/stop reason lists, offline data configuration, personalization
registration and recorder requests. Firmware timeout constants are unchanged
by these fixes. Run `check_services.py`, `check_audio.py` and
`check_hw_handshakes.py MEDIA` for protocol and hardware regression checks.

Button and touch input wakes the RCC peer immediately. Ethernet captures remain
buffered; set `MHI2_RCC_TRACE=1` for packet hex logs and per-frame capture flushes.
Graphics command capture, per-command logs and duplicate client frame dumps are
also opt-in; see `glforward/README.md`.

Additional cold-boot fixes:

- Tegra30 USB controllers now use the real EHCI register layout (capabilities at
  0x100, operational registers at 0x130), with UTMI/HSIC reset and clock gating.
  All three PHY stabilization timeouts are absent in the verified boot. This
  does not establish phone passthrough or Android Auto transport support.
- Navigation namespace 5006, key 500 contains a valid empty version-1 settings
  record (`000100ff`). The original persistence writer created it; the captured
  NOR persistence filesystem is saved as `navigation-defaults-persist.bin` in
  the analysis media directory and reused by `glforward/build_media.py`.
  This removes the 30-second Java settings-load wait without changing its code.
- Garage button-list requests receive an empty typed list with the request's
  transaction ID, eliminating the recurring six-second retry.
- RCC async exceptions now carry the original interface's RT_* request ID,
  rather than the wire method number, so HMI request timers can match them.
- Early native display configuration removes the cockpit `setUpdateRate` wait.
  Native map visibility/context still must be requested by the cockpit viewer;
  without that request the encoder can report an invalid displayable.

USB register references: [Linux Tegra USB PHY](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/phy/phy-tegra-usb.c)
and [Tegra30 device tree](https://raw.githubusercontent.com/torvalds/linux/master/arch/arm/boot/dts/nvidia/tegra30.dtsi).
Regression checks exercise all three controllers' clock/reset gating as well as
MC and IOC handshakes. `usb-native-timeouts.json` and the corresponding logs in
`qemu/evidence-timeouts/` record the verified absence of PHY, navigation
persistence, cockpit-rate and garage-list timeout messages.

Native media profile readiness is fixed by storing RCC namespace 678364556,
key 21 as integer 257 (coding state 1, profile 1), rather than a four-byte blob.
The native integer reader now succeeds and publishes readiness. Navigation reads
the corrected profile too, but its native readiness publication appears
unimplemented. Its five-second wait remains: navigation/HMI code is deliberately
unchanged at the user's request.

The APB DMA model resolves both native audio routing waits; live logs show both
routing threads completing and native speech producing nonzero PCM after PPCS
address translation was connected. Hardware regression checks cover transfer
contents, IRQ masking/acknowledgment, pause/resume, cyclic halves, and SMMU page
crossings. Register references: [Linux APB DMA driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/dma/tegra20-apb-dma.c)
and the Tegra30 device tree linked above. This models the attached MHI2 TDM
stream, not arbitrary APB peripherals or the complete AHUB mixer.

Known remaining waits include Marvell WLAN/BT SDIO initialization and connectivity
preconditions. Native SSE capture still occasionally reports 15 ms queue waits;
routing completion does not establish reliable real-microphone recognition.
The IDE probe also reports no EIDE interface. Firmware deadlines have not been
shortened. One experimental boot entered a kernel dump with `i2c-tegra2` as its
active process; its cause remains unresolved. Subsequent boots ran successfully.

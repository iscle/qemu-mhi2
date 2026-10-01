# Production RCC input experiment

The K3342 firmware UI accepts touch input through its original DSIKeyPanel
service. No Java input adapter or patched HMI event handler is used.

Path: host mouse → RCC service peer → ESO TCP over Ethernet → emulated TI
PCIe endpoint → original QNX `devnp-mib-mmx.so` DMA rings/MSI → original ESO
broker and Java `DSIKeyPanel` dispatcher → HMI.

With the prepared experimental media, launch everything with one command:

```sh
bash /home/iscle/qemu-mhi2/tools/mhi2/run_ui.sh
```

This starts QEMU, the graphics bridge, the RCC service peer, the MOST video receiver and the viewer.
Closing the viewer or pressing Ctrl-C stops the whole session. Use `--headless`
only when a desktop window is not wanted. Launcher output is saved to
`/tmp/mhi2-session.log`, and viewer errors to `/tmp/mhi2-viewer.log`.

The viewer requires PySide6 (installed here). Click the 800×480 screen or use
the physical controls arranged like the Volkswagen display: RADIO, MEDIA,
PHONE and VOICE on the left; NAV, TRAFFIC, CAR and MENU on the right. Click a
knob to press it, scroll over it to turn it, or use its −/+ buttons. The left
knob sends power/volume events; the right sends select/tune events. An additional
BACK button sits below the panel. The Virtual Cockpit button opens the MOST video surface (see [MOST.md](MOST.md)). Voice, navigation and other functions still
depend on their companion services being available in the emulation.
Startup takes several minutes of
host time. The image defaults to English (UK). Only one instance can use
these fixed FIFO/frame paths; the launcher rejects a second launcher session.
Stop any older standalone diagnostic VM before launching it. Disk writes use a
temporary snapshot by default.

Observed validation: three native guest ping replies, successful broker
registration and HMI key-panel subscription, touch opening FM settings, and
touch returning to the radio screen. The viewer's mouse handlers were also
tested with Qt's offscreen backend. Desktop window creation from the agent
session was denied by the environment; launch the viewer from your own terminal.

Evidence is in `/home/iscle/Downloads/mhi2-analysis/qemu/evidence-ui/`:
`rcc-touch-radio-before.png`, `rcc-touch-radio-settings.png`,
`rcc-viewer-click-settings.png`, and `input-mhi2-rcc*` captures/logs.
Recovered Java interfaces and handlers are in `qemu/protocol-analysis/`.
Native PCIe drivers were imported into the existing
`ghidra/project-v2/MHI2_Quickboot_DeepAnalysis.gpr` project.

The English default is stored by the firmware's own persistence service in
namespace 1101, key 10: a 22-byte blob containing `en_GB` followed by 17 spaces.
The guest-written PERSIST flash filesystem is saved as
`qemu/default-english-persist.bin`, with the seeding command and checksum in
`default-english-persist.json`. The experimental media builder restores this
seed when present. A fresh boot was verified to load `en_GB` automatically.
The former image is backed up as `k3342-gl-debug-nor.before-english.bin`.

## Wire details

MMX is 10.0.0.15, RCC is 10.0.0.16. The RCC peer registers agent 402's key-panel
service with broker port 21100 and accepts HMI traffic on 21402. ESO protocol
version 5 uses a four-byte big-endian length, one-byte message kind, and
big-endian serializer 1. DSIKeyPanel UUID is
`beff9a63-a8c2-503f-8909-dd7157fda6dc`, instance 0. Reply method 42 carries
`updateGesture2`, method 38 carries `updateKey2`, and method 30 carries signed
`updateEncoder2` increments. Physical controls use keyboard ID 13, as mapped by
the K3342 `configurationmanager.res` key table (not generic FCC ID 1); VOICE uses
key 50 (PTT), and select/volume encoders use keys 16/17. Touch keyboard ID is 13;
press/tap/release gesture codes are 4/1/3. Notification confirmation requires
bit 0x80 in the initial validity value; normal events use validity 1.

The PCIe endpoint identifies as TI 104c:b800. BAR0 contains application/DMA
window registers, BAR1 the RCC shared rings. Each ring has 32 descriptors of
28 bytes. Ethernet packets are copied through the actual guest buffers and
completion is signalled by MSI. Packet copies are limited to 2048 bytes and
outbound DMA cannot cross an 8 MiB window boundary. `check_devices.py` covers
enumeration, BAR sizing and ring publication; native ping and ESO captures
exercise actual DMA and interrupts.

## Limits

This is a local prototype, not full Jacinto emulation. The peer implements
ARP, ICMP echo, a minimal TCP subset, service registration and key-panel events.
TCP now segments responses, maintains a bounded send queue/window and retransmits.
Out-of-order arrivals are acknowledged without advancing the receive sequence;
this remains a limited TCP implementation. Recovered SWDL, diagnostic persistence
and vehicle DSI service models are described in [MISSING_MENU_DATA.md](MISSING_MENU_DATA.md).
Radio hardware, audio, navigation and vehicle functions are not thereby working.
The PCIe model is firmware-specific and lacks migration support.

Graphics use the separate experimental EGL/GLES replacements and host Mesa;
native Tegra GR3D is not implemented. See `glforward/README.md`. This validates
an interactive firmware UI, not a complete hardware-accurate head unit.

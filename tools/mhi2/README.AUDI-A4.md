# Audi A4 K3346 emulator additions (draft for maintainer review)

This branch adds the locally exercised Audi A4 B9 / AUG22 K3346 desktop
controller, display size and navigation experiments. Volkswagen remains the
default profile; Porsche and Audi A3 retain their firmware-specific wiring.
This is an AI-assisted contribution submitted explicitly for the fork owner's
review. The inherited QEMU AI-content policy is acknowledged; acceptance is
for the maintainer to decide. It is not represented as policy-compliant QEMU
upstream work.

## Included changes

- Audi fascia: NAV/MAP and TEL rocker, RADIO and MEDIA rocker, MENU, BACK,
  LEFT/RIGHT softkeys (98/97), CAR/VOICE/TRAFFIC and separate POWER/VOLUME.
- MMI Select knob, mouse wheel, upward curved rotation arrows (left clockwise
  +1, right counterclockwise -1), four joystick directions and neutral release.
- Audi A4 profile: 1024 x 480 guest/viewer dimensions, scalable framebuffer
  validation and coordinate conversion, keyboard type reply 32, 12 MHz clock.
  Automatic detection is limited to the tested K3346 release. Explicit profile
  selection also works without a metadata file in the standalone viewer.
- K3346-specific DSI schema and experimental MMI touch companion. English
  Map/Characters labels, Latin and shared Cyrillic block-shape candidates;
  explicit Send confirmation. This is a host geometric prototype, not the
  proprietary physical handwriting recognizer or cursive support.
- Native map joystick adapter, opt-in with MHI2_MAP_JOYSTICK=1: enter free-map
  mode, scroll in four directions and stop on release. Provider visibility
  guards hidden-map scrolling; delayed releases remain neutral.
- Map touch cursor overlay on a stationary framebuffer and native point-info
  request on release; stale context generations cancel interactions.
- K3346 vehicle identity: Audi brand nibble, AU491 class/generation/derivative
  coding. Previous local analysis connected the generic AU370 identity to
  the empty menu resource; AU491 selected the populated main menu. The same
  Audi identity is consumed by navigation. Other coding bytes are preserved.
- Graphics bridge size overrides and optional draw statistics; optional guest
  boot-command file, with no startup-timeout workaround applied.
- Opt-in P470 shader experiment: 42 GLSL translations for road/ground/sky,
  billboards, labels and POI rendering, translators and an RPC rendering check.

No Ukrainian localization, language-selection persistence override, language
hook, translation archive, keyboard modifications or Ukrainian-specific
handwriting glyphs are included. No firmware image, map database, captured
shader binary, NOR patch or owner-specific filesystem path is distributed.

## Selecting the interface

Use the existing firmware profile switch; there is no new appearance-only
switch which could accidentally change the guest's input wiring:

```sh
# Original default appearance and profile:
bash tools/mhi2/run_ui.sh --firmware vw
# Audi A4 with the owner's separately prepared matching media:
MHI2_MAP_JOYSTICK=1 MHI2_GL_WIDTH=1024 MHI2_GL_HEIGHT=480 \
  bash tools/mhi2/run_ui.sh --firmware audi-a4 --media /path/to/a4-media
# Standalone desktop viewer, without booting different firmware:
MHI2_FIRMWARE=vw python3 tools/mhi2/view_ui.py
MHI2_FIRMWARE=audi-a4 python3 tools/mhi2/view_ui.py
```

A4 launch requires matching prepared media. Build and stage the 1024 x 480
guest EGL/GLES libraries into a separate media copy using the existing media
preparation workflow; setting only the host window size is insufficient:

```sh
MHI2_BUILD_LIBRARIES_ONLY=1 MHI2_BUILD_DIR=/tmp/mhi2-a4-bridge \
  MHI2_GL_CFLAGS='-DMHI2_DEFAULT_W=1024 -DMHI2_DEFAULT_H=480' \
  bash tools/mhi2/glforward/build_local.sh
```

The main [build and boot guide](../../README.md#audi-a4-b9--k3346-experimental)
also shows the experimental full archive build, which automatically stages
these libraries; that fresh archive-to-boot workflow remains unverified.
For a manually prepared build, set MHI2_GLHOST to its mhi2-glhost when launching. Default bridge
builds remain 800 x 480. The experimental MHI2_NAV_MAP_TRANSMISSION override
accepts only 0..3; it does not prove Virtual Cockpit LVDS output.
`MHI2_GL_STATS=1` enables per-client draw diagnostics. `MHI2_BOOT_COMMANDS`
can name a local guest-command file appended to the diagnostic boot commands;
neither option supplies a navigation startup fix.

## Map rendering experiment

See [the shader experiment guide](experiments/audi-p470/README.md). This is an isolated, explicitly enabled
cache and is not selected automatically for VW or other firmware. The HMI
and navigation shader caches must be combined in a separate directory;
do not replace the original reviewed cache.

## Validation and limits

Current branch checks: 13 configuration tests; 26 service tests; offscreen
VW/Audi A3/Audi A4/Porsche controller and input routing; map joystick, touch
mode isolation, final contact/release, context switches and moving overlay.
The host bridge and 1024 x 480 ARM guest libraries build. The real RPC bridge
compiles 42/42 translated shaders and links 17 native pairs. Road blend pixel
(70,45,32,255), billboard pixel (43,26,100,255), transparent billboard preserves
(26,26,26,255). Checks used locally held shader captures, not distributed here.

```sh
python3 tools/mhi2/check_emulator_config.py
python3 tools/mhi2/check_services.py
QT_QPA_PLATFORM=offscreen python3 tools/mhi2/check_firmware_profiles.py
python3 tools/mhi2/check_map_joystick.py
QT_QPA_PLATFORM=offscreen python3 tools/mhi2/check_mmi_touch_modes.py
```

Earlier local screenshots showed a populated main menu and native roads,
labels/building footprints and POI icons. These were not rerun on an English
stock-media boot for this PR. Full visual acceptance, all map details and 3D
objects, perspective artifacts and first-navigation-start reliability remain
open. Prior 90/180-second deadlines and delayed-launch experiments did not
resolve the startup stall and were reverted; those patches are excluded.
Physical MMI, proprietary handwriting and Virtual Cockpit behavior are not
validated by the emulator. The previously used slow upload helper is not
bundled separately: upstream already contains the console upload facility.

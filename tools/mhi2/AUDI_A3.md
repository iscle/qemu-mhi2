# Audi A3 MHI2 P5089

The private `mhi2-audi-a3` branch adds the native Audi QNX HMI from
`MHI2_ER_AU37x_P5089_MU1326 (A3).7z` alongside VW K3342. It uses the firmware's
Quickboot 17.54, QNX kernel, app partition and unchanged Audi Java/EAL HMI.
This is prepared emulator media, not an image for flashing a head unit.

## Run

On this workspace:

```sh
bash /home/iscle/mhi2-audi-port/run.sh
```

The equivalent firmware-selecting command is:

```sh
bash /home/iscle/mhi2-audi-port/qemu/tools/mhi2/run_ui.sh --firmware audi-a3
```

Use `--firmware vw` for the existing Volkswagen media, or `--headless` for
console testing. `--media /path/to/media` selects another prepared directory;
its manifest must match the selected firmware train. The default Audi media is
`/home/iscle/mhi2-audi-port/media/audi-a3`. Closing the window stops the session.
The NOR/eMMC images run as temporary snapshots.

Audi uses its center-console controller (DSI FCC keyboard 1). The viewer shows
Menu, Radio, Media, Nav, Tel, Car, four context buttons, Back, and separate
volume and selection knobs. Scroll over a knob or use its minus/plus buttons;
click the selection knob to enter. Arrow keys turn the selection knob,
Enter selects, Esc goes back, and M opens Menu. Screen clicks do not generate
touch events. VW retains touchscreen input and ABT keyboard 13.

## Preparation

Keep the archive and previous VW media unchanged. Extract the Audi archive to
`/home/iscle/mhi2-audi-port/firmware`, then use `prepare.py` with that directory
and the previously captured variant-70 baseline NOR/IRAM/eMMC. Use
`extract_main_ifs.py` to unpack Audi's `mifs-stage2.img`.

Build the bridge libraries without invoking the old VW-specific media builder:

```sh
MHI2_BUILD_LIBRARIES_ONLY=1 bash tools/mhi2/glforward/build_local.sh
python3 tools/mhi2/prepare_ui.py \
  --base /home/iscle/mhi2-audi-port/media/base \
  --main-ifs /home/iscle/mhi2-audi-port/media/main-ifs \
  --app /home/iscle/mhi2-audi-port/firmware/MMX2/app/70/default/app.img \
  --formatted-disk /home/iscle/Downloads/mhi2-analysis/qemu/emmc-full.raw \
  --output /path/to/new-audi-media
```

The builder requires a new output directory. It copies the formatted data-disk
layout, replaces its app filesystem, installs matching compressed main IFS
copies in NOR and `/img_restore`, and adds the existing diagnostic shell/audio
endpoint and graphics bridge. It extracts the local Kanzi GLSL cache from the
user's app image. Firmware binaries, generated GLSL and media are not Git assets.

## Compatibility changes

* Audi's display driver needs the existing 12 MHz oscillator model. The launcher
  selects it only for Audi; VW retains its previous clock configuration.
* `EGL_NV_system_time` uses QNX's monotonic clock at one billion ticks per second.
  See the [EGL extension specification](https://registry.khronos.org/EGL/extensions/NV/EGL_NV_system_time.txt)
  and [QNX ClockTime documentation](https://www.qnx.com/developers/docs/6.5.0SP1/neutrino/lib_ref/c/clocktime.html).
* Client vertex/index arrays are copied from guest RAM into host buffers before
  drawing. Host EBO queries are bounded by the actual buffer size. Guest addresses
  are never passed to the host GL driver as client pointers.
* Kanzi material references pair each compiled Tegra program with its original
  GLSL, even when source payload order differs. NVIDIA blend pragmas become
  scoped host blending. Built-in EAL programs and the Audi compositor have
  separately identified translations; Tegra FX10 quantization is not reproduced.
* RCC input identity and update inventory follow the selected firmware.

## Validation and scope

Native Audi Menu, rotary selection, knob press, Back and Car were exercised;
English text and the A3 menu/car model render on the AMD GLES driver. The
existing VW image also boots with the updated host bridge, and a native
touchscreen tap opens its FM settings. The final Audi media was booted through
the single-command launcher with no unknown shader or link failures. Tests:

```sh
python3 tools/mhi2/check_firmware_profiles.py
python3 tools/mhi2/check_kanzi_shaders.py
python3 tools/mhi2/glforward/check_client_indices.py
/tmp/mhi2-glhost --check-shaders
```

This establishes boot/display/controller support. It does not establish parity
for every Audi service: media can remain on its initialization notice, vehicle
settings depend on the simulated RCC service data, and Audi navigation/map,
speech and Virtual Cockpit behavior have not been validated. The existing VW
media and firmware remain separate.

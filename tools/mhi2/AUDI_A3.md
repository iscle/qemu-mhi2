# Audi A3 MHI2 P5089

For a fresh Mac, use the [macOS setup and boot guide](../../README.md), including
its Audi A3 command. The workspace paths below are historical examples.

The unified `main` branch includes the native Audi QNX HMI from
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

## Reproduce on another computer

The build scripts support Linux and macOS, with Python 3.11 or newer, Mesa
EGL/OpenGL ES libraries, and an ordinary desktop session for the viewer. Linux
build and full Audi boot have been tested on Fedora. Fresh QEMU/bridge builds,
hardware video and a Porsche QNX boot have been validated locally on Apple
Silicon macOS; see [host validation](HOST_PARITY.md#validation). Full Audi boot
on macOS remains unverified. GitHub refused to start the portable-build jobs
because of the account billing/spending limit. Reserve about 25 GB
for a source tree, extracted firmware, build outputs and sparse disk
images. More space is needed if the destination filesystem does not preserve
sparse files. No QNX SDK, Ghidra, Pico SDK, Pico firmware, separate Pico
checkout or physical Pico device is required. The optional `usb-mhi2-pico`
test fixture uses an in-tree ASIX protocol implementation; ordinary head-unit
boot does not instantiate it.

### 1. Install dependencies and download the source

On Ubuntu 24.04:

```sh
sudo apt update
sudo apt install build-essential git pkg-config ninja-build python3-venv \
  python3-dev libglib2.0-dev libpixman-1-dev zlib1g-dev libfdt-dev \
  libffi-dev libegl-dev libgles-dev liblzo2-dev libusb-1.0-0-dev \
  libavcodec-dev libavutil-dev libswscale-dev p7zip-full zstd ffmpeg \
  sox libsox-fmt-all pipewire-bin \
  libxcb-cursor0 libxkbcommon-x11-0 gh curl
```

On macOS, install the Xcode Command Line Tools (`xcode-select --install`) and
[Homebrew](https://brew.sh), then:

```sh
brew install git pkgconf ninja python glib pixman mesa lzo libusb sevenzip zstd \
  ffmpeg sox gnu-tar gh
# Use GNU tar to preserve the sparse bootstrap image when extracting it.
export PATH="$(brew --prefix gnu-tar)/libexec/gnubin:$PATH"
```

The host bridge uses `pkg-config egl glesv2 libavcodec libavutil libswscale`
on both hosts. FFmpeg supplies the platform video backend. Both hosts support
native H.264 output to the center screen and cluster, crop/visibility controls,
accelerated encoding and playback/microphone input. See the
[host parity and validation notes](HOST_PARITY.md) for hardware selection,
fallbacks and tests. Mesa's selected renderer determines GLES acceleration;
installing Mesa alone is not a claim of GPU acceleration on every Mac.

On either host:

```sh
mkdir -p "$HOME/mhi2-repro"
cd "$HOME/mhi2-repro"
git clone --depth 1 --branch main https://github.com/iscle/qemu-mhi2.git qemu
cd qemu
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r tools/mhi2/requirements-audi.txt
```

Source ZIPs and shallow clones are also supported; see the
[macOS guide](../../README.md) for ZIP instructions. The builder fetches three
pinned C dependencies and applies QEMU's Meson overlays. The dependencies are
not vendored into this repository. Optional Rust devices are disabled, and
there is no need to build the ROM submodules. Initial setup needs access to
GitHub, GitLab and PyPI.

### 2. Install the tested ARM cross compiler

The guest compatibility libraries use **xPack GNU Arm Embedded GCC 13.3.1-1.1**.
They are freestanding ARM soft-float builds, not host Linux libraries. On a
supported Linux/macOS host:

```sh
mkdir -p ../toolchain
cd ../toolchain
case "$(uname -s)" in Darwin) system=darwin;; Linux) system=linux;; esac
case "$(uname -m)" in arm64|aarch64) arch=arm64;; x86_64) arch=x64;; esac
asset=xpack-arm-none-eabi-gcc-13.3.1-1.1-$system-$arch.tar.gz
gh release download v13.3.1-1.1 \
  --repo xpack-dev-tools/arm-none-eabi-gcc-xpack --pattern "$asset*"
shasum -a 256 -c "$asset.sha"
tar -xzf "$asset"
export PATH="$PWD/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin:$PATH"
cd ../qemu
arm-none-eabi-gcc --version
```

The upstream [toolchain release](https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/tag/v13.3.1-1.1)
provides Linux and Darwin x64/ARM64 assets. Only Linux x64 was run locally
for this portability change.
Alternatively pass `--arm-prefix /absolute/path/bin/arm-none-eabi-` to the
builder. Do not substitute `arm-linux-gnueabihf-gcc`.

### 3. Obtain the firmware and emulator bootstrap

Copy your original `MHI2_ER_AU37x_P5089_MU1326 (A3).7z` onto the new computer.
The archive used for validation has SHA-256:

```text
528817504220be40638e7fb08be0e9a20ef3051ab1ea292e9c2b9ecb49884bf9
```

**The update archive alone is insufficient for this boot harness.** It does
not supply the captured BootROM handoff/BIT/BCT, complete initial NOR layout,
or the native-formatted data-disk template used by this emulator. These are
provided in the [bootstrap-v1 release](https://github.com/iscle/qemu-mhi2/releases/tag/mhi2-bootstrap-v1),
so you do not need files from the original developer's computer:

```sh
mkdir -p ../bootstrap
cd ../bootstrap
gh release download mhi2-bootstrap-v1 --repo iscle/qemu-mhi2 \
  --pattern 'mhi2-bootstrap-v1.tar.zst' --pattern SHA256SUMS
shasum -a 256 -c SHA256SUMS
tar --zstd -xf mhi2-bootstrap-v1.tar.zst
cd ../qemu
```

The compressed bootstrap's SHA-256 is
`4c4e8eaa5a693dcfe9ece461bca302549f6f66fc1de191ccc36af11a67e4186e`.
It contains `nor.bin` (64 MiB), `iram.bin`, `emmc-template.raw` (4 GiB logical,
sparse), and `bootstrap.json` with per-file hashes and provenance. The app
partition is zero-filled. Audi app/kernel/Quickboot payloads are installed
from **your archive**, not supplied by the template. The other data partitions
retain the previously tested native QNX6 formatting. GNU tar preserves the
sparse disk on extraction. This bootstrap is emulator-only, not a flash image.

### 4. Build everything

From the checkout, with the virtual environment active:

```sh
python tools/mhi2/build_audi.py \
  --archive "/path/to/MHI2_ER_AU37x_P5089_MU1326 (A3).7z" \
  --bootstrap "$PWD/../bootstrap/bootstrap-v1" \
  --output "$PWD/../audi-build" \
  --jobs 8
```

Replace only the archive path. The output directory must not exist; existing
firmware, media and builds are never overwritten. If a build fails, inspect
the error and retry with a new output directory. Use fewer jobs on hosts with
limited RAM. The script checks bootstrap hashes, extracts the archive, builds
`qemu-system-arm`, builds the EGL/GLES/encoder/PCM bridge, assembles Audi NOR
and eMMC media, and extracts the 240-program local Kanzi shader cache. It copies
`metainfo2.txt` into the output media for runtime version reporting.

Outputs under `audi-build`:

| Path | Purpose |
| --- | --- |
| `qemu/qemu-system-arm` | Emulator built from this checkout |
| `bridge/` | Host graphics executable and ARM guest compatibility binaries |
| `firmware/` | Extracted update, left unchanged |
| `media/base/` | Quickboot/kernel/app installation before UI bridge replacement |
| `media/main-ifs/` | Expanded main IFS and extracted regular files |
| `media/ui/` | Matching NOR/IRAM/eMMC, metadata and local shader cache |
| `build-manifest.json` | Source commit, compiler, archive and bootstrap provenance |
| `run.sh` | Launcher using these newly built files |

No firmware archive, raw disk or generated proprietary GLSL is committed to
Git. The separate bootstrap release is required input data.

### 5. Boot and check

```sh
bash ../audi-build/run.sh
```

The generated launcher remembers the Python environment and source checkout,
and selects its own QEMU, host bridge, media and metadata. Keep the checkout
and its virtual environment in place. It does not read `/home/iscle` paths.
For a machine without a desktop window, use `--headless`; a working EGL driver
is still required. Do not run two sessions at once: the emulator uses shared
`/tmp/mhi2-*` transports protected by a session lock.

Expect the native English Audi UI, then use Menu and the rotary controller.
Logs are in `/tmp/mhi2-session.log`, `/tmp/mhi2-debug-console.log`,
`/tmp/mhi2-glhost.log` and `/tmp/mhi2-rcc-peer.log`; completed runs are archived
under `audi-build/media/runs/`. Each boot uses writable temporary snapshots.

Optional checks, run after closing the emulator:

```sh
python tools/mhi2/check_sparse_copy.py
python tools/mhi2/check_lzo_library.py
python tools/mhi2/check_firmware_profiles.py
python tools/mhi2/check_kanzi_shaders.py
MHI2_GLHOST="$PWD/../audi-build/bridge/mhi2-glhost" \
  python tools/mhi2/glforward/check_client_indices.py
../audi-build/bridge/mhi2-glhost --check-shaders
```

If EGL initialization fails, check the installed GPU driver and permissions on
`/dev/dri/renderD*`. `LIBGL_ALWAYS_SOFTWARE=1` is available for diagnostic runs.
If Qt reports an xcb plugin error, check the xcb/xkb packages above and run from
a desktop terminal. A VA-API encoder probe failure falls back to software
encoding; it does not mean the main UI is software-rendered.

### Individual preparation tools and bootstrap provenance

`build_audi.py` prints each command it runs. Its pipeline is `prepare.py`,
`extract_main_ifs.py`, and `prepare_ui.py --bridge-dir <build>/bridge`, after
compiling the bridge with `MHI2_BUILD_LIBRARIES_ONLY=1` and
`MHI2_BUILD_DIR=<build>/bridge`. This avoids the older VW-specific media builder.
The builder installs identical compressed main IFS copies in NOR and the app's
`/img_restore`, adds the diagnostic shell/audio endpoint, and replaces the
EGL/GLES/encoder libraries. The Audi Java/EAL HMI remains unchanged.

`export_bootstrap.py` records how the versioned asset was produced from the
previously tested emulator captures. It reads the original NOR, IRAM and
formatted disk, creates separate output files, removes the app partition
contents, and records source/output SHA-256 hashes. It is for regenerating the
bootstrap from those original inputs; it is **not** an archive-only substitute
for them. The download above is the supported input on a separate computer.

The portable builder was also checked from a fresh checkout using the
downloaded `bootstrap-v1` asset and the original archive. It completed the
QEMU/bridge/media build and the generated launcher reached the native English
Audi radio screen. This validates the reproduction path on the tested Fedora
host; the Ubuntu package example has not been tested in a separate VM.
The `MHI2 portable build` workflow builds QEMU, host/guest bridges and runs
firmware-free protocol, shader and image-preparation checks on Ubuntu and
macOS. It runs the same real H.264 frame/plane tests, encoder round trip and audio
contract checks on both hosts; ordinary CI uses software codecs and does not
require a physical GPU or microphone. Actions must be enabled and the account billing limit
resolved before this workflow can validate the macOS path.

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

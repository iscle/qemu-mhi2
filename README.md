# MHI2 head-unit emulator

Boot supported **Audi, Volkswagen and Porsche MHI2 firmware** on a Mac, with
the original QNX user interface and emulated vehicle/companion services.
This is an experimental QEMU fork. It does not require a head unit,
Raspberry Pi Pico, QNX SDK or a separate Pico repository.

## Supported firmware

Supply your own supported firmware update archive; the repository does not
include firmware updates or navigation maps.

The supported profiles are specific firmware trains, not every update for a
brand:

| Profile | Firmware train | MU | Controls |
| --- | --- | --- | --- |
| `audi-a3` | `MHI2_ER_AU37x_P5089` | 1326 | MMI rotary controller, context keys; no touchscreen |
| `vw` | `MHI2_ER_VWG11_K3342` | 1427 | Touchscreen and VW side keys/knobs |
| `porsche` | `MHI2_ER_POG11_K5126` | 1394 | Touchscreen and Porsche PCM fascia |

Use variant-70 update assets for these profiles. The builder checks the
archive's `[common]` release against the selected profile and rejects a
mismatch. Porsche P5250, POG24 K4196 and other trains are **not** substitutes
for the K5126 command below.

Apple Silicon macOS has been tested locally. Intel Macs have a toolchain
selection path but have not been validated here. Allow at least 25 GB of free
space per build, plus room for your archives; large firmware packages need
more. Keep the checkout and build directories after building: the generated
launcher uses both.

## 1. Set up macOS

Install Apple's command-line tools if needed, and complete the installer:

```sh
xcode-select --install
```

Install [Homebrew](https://brew.sh/) if it is not already available, then run:

```sh
brew install git pkgconf ninja python glib pixman mesa lzo libusb sevenzip zstd \
  ffmpeg sox gnu-tar

export MHI2_REPRO="$HOME/mhi2-repro"
mkdir -p "$MHI2_REPRO"
curl -fL https://github.com/iscle/qemu-mhi2/archive/refs/heads/main.zip \
  -o "$MHI2_REPRO/qemu.zip"
unzip -q "$MHI2_REPRO/qemu.zip" -d "$MHI2_REPRO"
mv "$MHI2_REPRO/qemu-mhi2-main" "$MHI2_REPRO/qemu"
cd "$MHI2_REPRO/qemu"
"$(brew --prefix python)/bin/python3" -m venv .venv
. .venv/bin/activate
python -m pip install -r tools/mhi2/requirements-audi.txt
```

Despite its historical name, `requirements-audi.txt` supplies the Python
requirements for all three profiles. Use Python 3.11 or newer. **Code → Download
ZIP**, the ZIP command above, and Git clones (including shallow clones) are all
supported. If you prefer Git, replace the download/unzip/move commands with:

```sh
git clone --depth 1 --branch main https://github.com/iscle/qemu-mhi2.git "$MHI2_REPRO/qemu"
```

The ZIP contains source, so it still needs to be compiled. Git is used only to
fetch pinned third-party build dependencies; your QEMU directory needs no
`.git` metadata or history. First-time setup needs internet access to GitHub,
GitLab and Python packages.

Install the ARM compiler used for the guest graphics libraries:

```sh
mkdir -p "$MHI2_REPRO/toolchain"
cd "$MHI2_REPRO/toolchain"
case "$(uname -m)" in arm64) arch=arm64;; x86_64) arch=x64;; esac
asset="xpack-arm-none-eabi-gcc-13.3.1-1.1-darwin-$arch.tar.gz"
release="https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v13.3.1-1.1"
curl -fLO "$release/$asset"
curl -fLO "$release/$asset.sha"
shasum -a 256 -c "$asset.sha"
tar -xzf "$asset"
export PATH="$MHI2_REPRO/toolchain/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin:$PATH"
arm-none-eabi-gcc --version
cd "$MHI2_REPRO/qemu"
```

The compiler must be `arm-none-eabi`, not `arm-linux-gnueabihf`. You may use an
already installed compiler by passing `--arm-prefix /absolute/path/bin/arm-none-eabi-`
to the build command. GNU Arm 14.3.Rel1 has also built the Porsche guest bridge
on Apple Silicon.

## 2. Download the emulator bootstrap

The update archive is not a complete flash image. All three workflows also
need the captured boot handoff and formatted disk template from the
[bootstrap release](https://github.com/iscle/qemu-mhi2/releases/tag/mhi2-bootstrap-v1):

```sh
mkdir -p "$MHI2_REPRO/bootstrap"
cd "$MHI2_REPRO/bootstrap"
release="https://github.com/iscle/qemu-mhi2/releases/download/mhi2-bootstrap-v1"
curl -fLO "$release/mhi2-bootstrap-v1.tar.zst"
curl -fLO "$release/SHA256SUMS"
shasum -a 256 -c SHA256SUMS
gtar --zstd -xf mhi2-bootstrap-v1.tar.zst
cd "$MHI2_REPRO/qemu"
```

GNU tar preserves the sparse disk. The extracted directory
`$MHI2_REPRO/bootstrap/bootstrap-v1` must contain `bootstrap.json`, `nor.bin`,
`iram.bin` and `emmc-template.raw`. The builder checks their hashes. The app
partition in the template is empty; the selected firmware supplies the app,
Quickboot, kernel and system files. This bootstrap is for emulation only.

## 3. Build and boot your firmware

Run the following from `$MHI2_REPRO/qemu` with `.venv` activated and the ARM
compiler on `PATH`. Replace the archive path with your actual file. Each build
creates its own QEMU executable, host/guest bridges, prepared NOR/eMMC images
and `run.sh`. **The output directory must not already exist.**

### Audi A3

```sh
python tools/mhi2/build_firmware.py --firmware audi-a3 \
  --archive "/path/to/MHI2_ER_AU37x_P5089_MU1326 (A3).7z" \
  --bootstrap "$MHI2_REPRO/bootstrap/bootstrap-v1" \
  --output "$MHI2_REPRO/audi-build" --jobs 6
bash "$MHI2_REPRO/audi-build/run.sh"
```

Use the displayed MMI buttons and controller. Arrow keys turn the selection
knob, Enter selects, Esc goes back and M opens Menu. Click or scroll over the
knobs. Screen clicks do not send touches for Audi. Its launcher selects the
required 12 MHz oscillator automatically.

### Volkswagen

```sh
python tools/mhi2/build_firmware.py --firmware vw \
  --archive "/path/to/your-MHI2_ER_VWG11_K3342-update.7z" \
  --bootstrap "$MHI2_REPRO/bootstrap/bootstrap-v1" \
  --output "$MHI2_REPRO/vw-build" --jobs 6
bash "$MHI2_REPRO/vw-build/run.sh"
```

Click the touchscreen or use the VW side buttons. Scroll over the volume and
selection knobs to turn them. The profile retains VW's touchscreen wiring and
clock configuration.

### Porsche PCM

```sh
python tools/mhi2/build_firmware.py --firmware porsche \
  --archive "/path/to/MHI2_ER_POG11_K5126_MU1394_9P1906961H.7z" \
  --bootstrap "$MHI2_REPRO/bootstrap/bootstrap-v1" \
  --output "$MHI2_REPRO/porsche-build" --jobs 6
bash "$MHI2_REPRO/porsche-build/run.sh"
```

Use the touchscreen and Porsche TUNER, MEDIA, NAV, CAR and HOME controls. The
cluster window uses the smaller Porsche viewport. A cluster window being
available does not guarantee map/video output: it requires the corresponding
native service and data. This boot guide does not install an Android Auto mod
or enable a physical car's cluster.

### Subsequent launches and shutdown

Close the viewer or press Ctrl-C in its Terminal to stop the session and its
helpers. Run **one emulator session at a time**; the helpers share local FIFO,
port and log names. The launcher uses temporary disk snapshots by default, so
guest changes are discarded on exit and the prepared images remain reusable.
Do not move the checkout or build directory without rebuilding the launcher.

In a new Terminal, you can boot an existing build without rebuilding:

```sh
export MHI2_REPRO="$HOME/mhi2-repro"
bash "$MHI2_REPRO/porsche-build/run.sh"  # or audi-build / vw-build
```

For console-only testing, add `--headless`. The generated launcher selects
its build's QEMU, graphics host, Python environment, firmware metadata and
media paths; no paths from the original developer's computer are needed.

## What to expect

Cold boot can take several minutes. Porsche's Home page can remain at
**Loading Home** while services initialize, even after the radio screen works.
An empty cluster or unavailable navigation is expected without matching map
data; this guide does not download maps or speech/online-service packages.
The bootstrap's formatted data partitions are a starting point, not a complete
vehicle filesystem dump.

The shared host supports H.264 hardware encoding/decoding through VideoToolbox
on macOS. To require it and fail rather than fall back to software:

```sh
MHI2_ENCODER=hardware MHI2_DECODER=hardware \
  bash "$MHI2_REPRO/porsche-build/run.sh"
```

Mesa provides EGL/GLES rendering. The tested Mac uses **llvmpipe software GLES**;
hardware video decoding does not imply hardware-accelerated 3D rendering.
CoreAudio playback uses SoX; microphone use may require macOS permission.
See [host backend validation](tools/mhi2/HOST_PARITY.md) for the exact tests.

Audi and VW have earlier Linux boot evidence. Porsche K5126 and the shared
host backends have local Apple Silicon macOS execution evidence. On 2026-10-07,
the Porsche command was tested with a new Python environment, the published
bootstrap and original K5126 archive: a clean build and the generated launcher
reached the native radio screen. That run used GNU Arm 14.3.Rel1. Audi and VW
were not freshly boot-tested on macOS for this guide. This is not a
claim that every firmware, host architecture, vehicle service or Android Auto
connection works. Original HMI executables are used with emulator compatibility
libraries and a diagnostic shell in disposable media. Do not flash those
images onto a real head unit.

## Troubleshooting

- **Wrong firmware train:** use the exact train in the table; renaming an
  archive or editing its manifest does not make it compatible.
- **Output already exists:** choose a new `--output` path. A failed build is
  retained for inspection rather than overwritten.
- **Missing EGL/FFmpeg/LZO/USB libraries:** check
  `pkg-config --exists egl glesv2 libavcodec libavutil libswscale libusb-1.0`.
  Use a native Homebrew installation matching your Mac's architecture. A custom
  FFmpeg installation needs its `lib/pkgconfig` directory in `PKG_CONFIG_PATH`.
- **No window / helper exited:** read `/tmp/mhi2-session.log`,
  `/tmp/mhi2-viewer.log`, `/tmp/mhi2-debug-host.log`, `/tmp/mhi2-debug-console.log`
  and `/tmp/mhi2-glhost.log`. Full logs are copied to the build's `media/runs/`
  when the session closes.
- **A session is already running:** close its viewer or stop its Terminal
  before starting another firmware profile.

For advanced details see the [Audi reproduction guide](tools/mhi2/AUDI_A3.md),
[emulator internals](tools/mhi2/README.md), [fork notes](README.MHI2.md)
and the original [upstream QEMU README](README.rst).

## Source and licensing

The fork is based on [official QEMU](https://github.com/qemu/qemu). MHI2-specific
changes are experimental and include AI-assisted work; they are not intended
as upstream QEMU submissions. QEMU and third-party source retain their existing
copyright and license notices; see [COPYING](COPYING) and [COPYING.LIB](COPYING.LIB).
Firmware and captured bootstrap data are separate from the emulator source;
the source license does not grant rights to redistribute those assets.

# MHI2 Tegra3 emulation

Upstream QEMU `81ce3a87737aa50716c42db8886082d12783e0a1` was merged into
`mhi2-mainline-boot` in `/home/iscle/qemu-mhi2`. The original mounted checkout
and firmware captures have not been modified.

## Run the prepared K3342 firmware

```sh
/home/iscle/qemu-mhi2/tools/mhi2/run.sh \
  /home/iscle/Downloads/mhi2-analysis/qemu/k3342-70
```

UART D is the console. Ctrl-A C switches to the QEMU monitor; Ctrl-A X exits.
The eMMC uses a temporary snapshot. The NOR backing file is opened read-only.
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

This is an early userspace boot, not a working headunit HMI or interactive
QNX login. Remaining observed failures include `/mnt/app` mount failure and
`/dev/i2c5` not appearing, which prevents the PMU power driver from starting.
The NOR filesystem driver subsequently faults with SIGBUS, and display/capture
and PCIe services cannot initialize their missing hardware. The I2C model lacks
master packet mode. Tegra SDHCI vendor registers are still
unimplemented, and a CMD1 issued after Quickboot leaves the eMMC in ready state
is rejected by the generic card model. The A9 global timer still uses QEMU's
fixed 100 MHz model, whereas the bootloader configures a different CPU clock;
timeouts take substantially longer than expected. Allow several minutes to
observe the later startup messages. No HMI, audio, GPU or complete companion-MCU
emulation is claimed.

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
The longer logged run uses the prepared eMMC snapshot and records its mount
failure as well as successful loading of the main userspace image from NOR.

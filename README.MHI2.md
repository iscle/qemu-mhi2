# Private MHI2 emulator experiment

This is a source snapshot of the custom MHI2 QEMU tree, including the QNX HMI,
RCC/IOC services, host graphics/audio bridges, MOST virtual cockpit transport,
accelerated encoding, and the additional Tegra3 clock/timer changes used by Linux.

Start with [the emulator documentation](tools/mhi2/README.md).
The Linux payload build and emulator media are in
[iscle/mhi2-linux-port](https://github.com/iscle/mhi2-linux-port).
Firmware analysis and the unified Ghidra project are in
[iscle/mhi2-firmware-analysis](https://github.com/iscle/mhi2-firmware-analysis).

The snapshot includes local changes after original QEMU checkout commit
`bddb80a9770502c5fa26e933bd96c139ea0cd19b`, which merged upstream QEMU
`81ce3a87737aa50716c42db8886082d12783e0a1`.
The three populated softfloat/testfloat/keycodemapdb subprojects are vendored
so the tested build can configure with `--disable-download`.

This repository records a private experiment, including AI-assisted changes.
It is not intended as an upstream QEMU contribution. Existing license and
copyright notices remain applicable. Source history before this snapshot is
not included; provenance is recorded above.

Some QNX launch scripts still default to the original workstation's media paths.
The source snapshot includes those scripts and their documented overrides;
it does not include the large map database or all intermediate emulator disks.
The Linux repository release includes the exact media needed for its tested boot.

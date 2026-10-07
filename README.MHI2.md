# Private MHI2 emulator experiment

This is the custom MHI2 QEMU tree with its available source history, including the QNX HMI,
RCC/IOC services, host graphics/audio bridges, MOST virtual cockpit transport,
accelerated encoding, and the additional Tegra3 clock/timer changes used by Linux.

The primary branch is `main`. It combines the Porsche/VW `mhi2-quickboot`
and Audi `mhi2-audi-a3` histories without squashing either branch. Both old
branch tips remain available as archive tags. See
[consolidation and validation](tools/mhi2/CONSOLIDATION.md).

Start with [the emulator documentation](tools/mhi2/README.md) or the
[Linux/macOS build and Audi boot instructions](tools/mhi2/AUDI_A3.md#reproduce-on-another-computer).
Pico firmware, hardware and a separate Pico source checkout are not required.
The Linux payload build and emulator media are in
[iscle/mhi2-linux-port](https://github.com/iscle/mhi2-linux-port).
Firmware analysis and the unified Ghidra project are in
[iscle/mhi2-firmware-analysis](https://github.com/iscle/mhi2-firmware-analysis).

The original checkout history through commit
`bddb80a9770502c5fa26e933bd96c139ea0cd19b`, which merged upstream QEMU
`81ce3a87737aa50716c42db8886082d12783e0a1`, is retained as a parent of
the history-reconciliation merge. The earlier private snapshot commit and
its published ancestry are also preserved.
The three populated softfloat/testfloat/keycodemapdb subprojects are vendored
so the tested build can configure with `--disable-download`.

This repository records a private experiment, including AI-assisted changes.
It is not intended as an upstream QEMU contribution. Existing license and
copyright notices remain applicable. Clone without `--depth` to retain the
full available QEMU and local development history.

Some QNX launch scripts still default to the original workstation's media paths.
The source snapshot includes those scripts and their documented overrides;
it does not include the large map database or all intermediate emulator disks.
The Linux repository release includes the exact media needed for its tested boot.

Linux bring-up additionally exercises native PCIe RCC Ethernet (DMA/MSI),
all five I2C masters, all three EHCI controllers, read-only NOR, GPIO and RTC.
The `pmic=tps65911` and `pmic=max20024` machine options expose experimental
power-register models. Their OTP defaults and interrupt connection are test
fixtures, not production board wiring. The default `pmic=legacy` and 13 MHz
oscillator retain the previous QNX configuration. Linux test launchers select
`-global tegra30-clk.oscillator-12mhz=on` explicitly for the current PLLE table.
See the Linux repository for profile fixtures, limitations and native tests.
